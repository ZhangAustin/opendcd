//Main decoding command
#include <fstream>
#include <iostream>
#include <string>

#include <fst/extensions/far/far.h>

#include <dcd/arc-decoder.h>
#include <dcd/cascade.h>
#include <dcd/config.h>
#include <dcd/cpu-stats.h>
#include <dcd/generic-transition-model.h>
#include <dcd/hmm-transition-model.h>
#include <dcd/lattice-arc.h>
#include <dcd/log.h>
#include <dcd/memdebug.h>
#include <dcd/on-the-fly-rescoring-lattice.h>
#include <dcd/utils.h>


using namespace std;
using namespace dcd;
using namespace fst;

string lingware;
string tm_type = "generic";
string word_symbols_file;
string logfile = "/dev/stderr";

//Simple table writer for Kaldi FST tables
template <class A>
class TableWriter {
 public:
  typedef A Arc;

  // Creates a new (empty) FST table; returns NULL on error.
  static TableWriter* Create(const string& filename) {
    ofstream* strm = new ofstream(filename.c_str());
    if (strm->is_open())
      return new TableWriter(strm);
    return 0;
  }
  virtual void Add(const string& key, const Fst<A>& fst) {
    (*strm_) << key;
    (*strm_) << char(32);
    fst.Write(*strm_, FstWriteOptions());
  }

  virtual ~TableWriter() { 
    if (strm_)
      delete strm_;
  }

 private:
  TableWriter(ostream* strm) : strm_(strm) { }
  ostream* strm_;
  DISALLOW_COPY_AND_ASSIGN(TableWriter);
};


//B is the output lattice semiring
template<class TransModel, class B>
int ArcDecoderMain(ParseOptions &po, SearchOptions *opts, 
    const string &word_symbols_file) {
  typedef typename TransModel::FrontEnd FrontEnd;
  typedef OnTheFlyRescoringLattice L;
  typedef ArcDecoder<StdFst, TransModel, L> Decoder;
  PROFILE_BEGIN(ModelLoad);
  string trans_model_rs = po.GetArg(1);
  string fst_rs = po.GetArg(2);
  string feat_rs = po.GetArg(3);
  string out_ws = po.GetArg(4);
  Logger logger("dcd-decode", std::cerr, opts->colorize);
  logger(INFO) << "Decoder type : " << Decoder::Type();
  
  FarWriter<B>* farwriter = 
    FarWriter<B>::Create(out_ws, fst::FAR_DEFAULT);
  if (!farwriter)
    logger(FATAL) << "Failed to create far writer : " << out_ws;

  cerr << endl;

  Cascade<StdArc>* cascade = 0;
  TransModel* trans_model = 0;
  SymbolTable* wordsyms  = 0;

  logger(INFO) << "Attempting to read fst " << fst_rs;
  cascade = Cascade<StdArc>::Read(fst_rs);
  if (!cascade) 
    logger(FATAL) << "Failed to load fst from file : " << fst_rs;
  cerr << endl;

  logger(INFO) << "Attempting to read transition model " <<  trans_model_rs;
  trans_model = TransModel::ReadFsts(trans_model_rs,
                                                 opts->trans_scale);
  trans_model->DumpInfo(logger);
  if (!trans_model)
    logger(FATAL) << "Failed to read transition model";
  wordsyms = 0;
  if (!word_symbols_file.empty()) {
    logger(INFO) << "Attempting to read word symbols from " 
      << word_symbols_file;
    wordsyms = SymbolTable::ReadText(word_symbols_file);
    opts->wordsyms = wordsyms;
  }
  PROFILE_END();

  logger(INFO) << "Attempting to read features from " << feat_rs;
  SequentialBaseFloatMatrixReader feature_reader(feat_rs, true);
  Timer timer;
  int total_num_frames = 0;
  double total_time = 0.0f;

  if (g_dcd_memdebug_enabled)
    logger(INFO) << "Memory allocated before decoding : " 
                 <<  g_dcd_current_num_allocated / kMegaByte << " MB";
  
  CPUStats cpustats;
  //These need to be here as the each call returns the usage between
  //calls to the function
  cpustats.GetCurrentProcessCPULoad();
  cpustats.GetSystemCPULoad();
 
  StdFst *fst = 0;
  Decoder *decoder = 0;  
  for (int num = 0; !feature_reader.Done(); feature_reader.Next(), ++num) {
    if (num % opts->fst_reset_period == 0) {
      logger(INFO) << "Rebuilding cascade and decoder at utterance : " << num;
      if (decoder) {
        delete decoder;
        decoder = 0;
      }
      fst = cascade->Rebuild();
      if (!fst)
        logger(FATAL) << "Cascade build failed";
      if (fst->Start() == kNoStateId) 
        logger(FATAL) << "Fst does not have a valid start state";
      decoder = new Decoder(fst, trans_model, *opts);
    }
    const string& key = feature_reader.Key();
    opts->source = key;
    const Matrix<float>& features = feature_reader.Value();
    int frame_count = features.NumRows();
    logger(INFO) << "Decoding features : " << key << ", # frames " 
                 << frame_count;

    FrontEnd frontend(features, 1.0f);
    VectorFst<B> ofst;
    VectorFst<B> lattice;
    trans_model->SetInput(&frontend, *opts);
    timer.Reset();
    float cost = decoder->Decode(trans_model, *opts, &ofst, 
        opts->gen_lattice ? &lattice : 0);
    double elapsed = timer.Elapsed();
    stringstream farkey;
    farkey << setfill('0') << setw(5) << num << "_" << key;
    farwriter->Add(farkey.str(), opts->gen_lattice ? lattice : ofst);
    stringstream ss;
    int numwords = 0;
    for (StateIterator<Fst<B> > siter(ofst); !siter.Done(); siter.Next()) {
      ArcIterator<Fst<B> > aiter(ofst, siter.Value()); 
      if (!aiter.Done()) {
        const B &arc = aiter.Value();
        if (arc.olabel) {
          if (wordsyms) {
            ++numwords;
            const string &word = wordsyms->Find(arc.olabel);
            if (word.empty()) {
              logger(WARN) << "Missing word sym : " << arc.olabel;
              ss << "MISSING_WORD_SYM ";
            } else {
              ss << word << " ";
            }
          }
        }
      }
    }
    string recogstring = ss.str();
    ss.str("");

    ss << "Finished decoding : " << endl
       << "\t\t  Best cost : " << cost << endl 
       << "\t\t  Average log-likelihood per frame : " 
       << cost / frame_count << endl
       << "\t\t  Decoding time : " << elapsed << endl
       << "\t\t  RTF : " << (elapsed * 100.0 / frame_count) << endl
       << "\t\t  Number of words : " << numwords << endl
       << "\t\t  Recog result : (" << key << ") " 
       << recogstring << endl
       << "\t\t  Process CPU load : " 
       << cpustats.GetCurrentProcessCPULoad() << endl
       << "\t\t  System CPU load : " 
       << cpustats.GetSystemCPULoad() << endl
       << "\t\t  Peak memory usage : " 
       << GetPeakRSS() / (1024 * 1024) << " MB" << endl
       << "\t\t  Current memory usage : " 
       << GetCurrentRSS() / (1024 * 1024) << " MB" << endl;
    if (g_dcd_memdebug_enabled)
      ss << "\t\t  Memory allocated after decoding : " 
         <<  g_dcd_current_num_allocated / kMegaByte << " MB" << endl;
    logger(INFO) << ss.str();
    total_time += elapsed;
    total_num_frames += frame_count;
    feature_reader.FreeCurrent();
  }
  logger(INFO) << "Decoding summary : " << endl
    << "\t\t  Average RTF : " 
    <<  total_time * 100 / total_num_frames << endl
    << "\t\t  Peak memory usage  : " 
    << GetPeakRSS() / (1024 * 1024) << " MB" << endl
    << "\t\t  Total # of frames : " << total_num_frames << endl
    << "\t\t  Total decoding time : " << total_time << endl;

  PROFILE_BEGIN(ModelCleanup);
  if (farwriter)
    delete farwriter;
  if (decoder)
    delete decoder;
  if (cascade)
    delete cascade;
  if (trans_model)
    delete trans_model;
  if (wordsyms)
    delete wordsyms;
  PROFILE_END();
  return 0;
}

DECLARE_int32(v);

struct DecodeMainEntryBase {
  string desc;
  const string Description() {
    return "something";
  }
  virtual int Run(ParseOptions &po, SearchOptions *opts, 
                  const string &word_symbols_file) = 0;
  virtual ~DecodeMainEntryBase() = 0;
};

DecodeMainEntryBase::~DecodeMainEntryBase() {
}

template<class T, class B>
struct DecodeMainEntry : public DecodeMainEntryBase {
  virtual int Run(ParseOptions &po, SearchOptions *opts, 
                  const string &word_symbols_file) {
    return ArcDecoderMain<T, B>(po, opts, word_symbols_file);
  }
};

unordered_map<string, DecodeMainEntryBase*> main_map;

void ClearMainRegister() {
  for (unordered_map<string, DecodeMainEntryBase*>::iterator it 
      =  main_map.begin(); it != main_map.end(); ++it ) {
    delete it->second;
  }
  main_map.clear();
}

DecodeMainEntryBase* FindEntry(const string& key) {
  unordered_map<string, DecodeMainEntryBase*>::iterator it = 
    main_map.find(key);
  if (it == main_map.end()) { 
  #ifdef HAVEDL
    logger(INFO) << "Attempting to load shared object : " << key << "-dcd.so";
    void* handle = dlopen(key + "-dcd.so", RTLD_LAZY);
    if (!handle)
      logger(FATAL) << "FindEntry : " << dlerror();
    DecodeMainEntryBase* entry = FindEntry(key);
    if (!entry)
      logger(FATAL) <<"Entry lookup failed in : " << key << "-dcd.so";
    return entry;
  #endif
    return 0;
  } else {
    return it->second;
  }
}

template<class T, class B>
struct DecodeMainRegisterer {
  explicit DecodeMainRegisterer(const string& name) {
    main_map[name] = new DecodeMainEntry<T, B>;
  }
};

#define REGISTER_DECODER_MAIN(S, T, D, B) \
  static DecodeMainRegisterer<T<D>, B> \
    decode_registerer ## _ ## T ## _ ## D ## _ ## B(S);

REGISTER_DECODER_MAIN("hmm", HMMTransitionModel, Decodable, StdArc);
REGISTER_DECODER_MAIN("hmm_kaldi_lattice", HMMTransitionModel, Decodable, KaldiLatticeArc);
REGISTER_DECODER_MAIN("hmm_hitstats", HMMTransitionModel, SimpleDecodableHitStats, StdArc);
REGISTER_DECODER_MAIN("generic", GenericTransitionModel, Decodable, StdArc);


int main(int argc, char *argv[]) {
  PROFILE_FUNC();
  g_dcd_global_allocated = g_dcd_current_num_allocated;
  const char *usage = "Decode some speech\n"
        "Usage: dcd-decode [options] trans-model-in (fst-in|fsts-rspecifier) "
        "features-rspecifier far-wspecifier";
  
  PrintVersionInfo();
  cerr << "Program arguments : " << endl;
  ParseOptions po(usage);
  SearchOptions opts;
  po.Register("tmtype", &tm_type, "Type of transition model to use");
  po.Register("word_symbols_table", &word_symbols_file, "");
  po.Register("lingware", &lingware, "");
  po.Register("logfile", &logfile, "/dev/stderr");
  /*po.Register("wfst");
  po.Register("trans_model");
  po.Register("input");
  po.Register("output");*/
  opts.Register(&po);
  po.Read(argc, argv);
 
  if (po.NumArgs() != 4) {
    po.PrintUsage();
    cerr << "Built-in decoder types : " << endl;
    for (unordered_map<string, DecodeMainEntryBase*>::iterator it 
        =  main_map.begin(); it != main_map.end(); ++it ) {
      cerr << "  " << it->first << " : " << it->second->Description() << endl;
    }
    exit(1);
  }

  opts.Check();
  cerr << endl << "Search options : " << endl << opts << endl;
 
  DecodeMainEntryBase* runner = FindEntry(tm_type);
  if (!runner) 
    logger(FATAL) << "Unknown decoder type : " << tm_type;
  runner->Run(po, &opts, word_symbols_file);
   
  ClearMainRegister();
  PrintMemorySummary();

  PROFILE_UPDATE(); // update all profiles
  PROFILE_OUTPUT(NULL); // print to terminal
}
