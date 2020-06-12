// Copyright 2019 Alpha Cephei Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "kaldi_recognizer.h"
#include "json.h"
#include "fstext/fstext-utils.h"
#include "lat/sausages.h"

using namespace fst;
using namespace kaldi::nnet3;

KaldiRecognizer::KaldiRecognizer(Model *model, float sample_frequency) : model_(model), spk_model_(0), sample_frequency_(sample_frequency) {

    model_->Ref();

    feature_pipeline_ = new kaldi::OnlineNnet2FeaturePipeline (model_->feature_info_);
    silence_weighting_ = new kaldi::OnlineSilenceWeighting(*model_->trans_model_, model_->feature_info_.silence_weighting_config, 3);

    g_fst_ = NULL;
    decode_fst_ = NULL;

    if (!model_->hclg_fst_) {
        if (model_->hcl_fst_ && model_->g_fst_) {
            decode_fst_ = LookaheadComposeFst(*model_->hcl_fst_, *model_->g_fst_, model_->disambig_);
        } else {
            KALDI_ERR << "Can't create decoding graph";
        }
    }

    decoder_ = new kaldi::SingleUtteranceNnet3Decoder(model_->nnet3_decoding_config_,
            *model_->trans_model_,
            *model_->decodable_info_,
            model_->hclg_fst_ ? *model_->hclg_fst_ : *decode_fst_,
            feature_pipeline_);

    frame_offset_ = 0;
    round_offset_ = 0;
    input_finalized_ = false;
    spk_feature_ = NULL;

    InitRescoring();
}

KaldiRecognizer::KaldiRecognizer(Model *model, float sample_frequency, char const *grammar) : model_(model), spk_model_(0), sample_frequency_(sample_frequency)
{
    model_->Ref();

    feature_pipeline_ = new kaldi::OnlineNnet2FeaturePipeline (model_->feature_info_);
    silence_weighting_ = new kaldi::OnlineSilenceWeighting(*model_->trans_model_, model_->feature_info_.silence_weighting_config, 3);

    g_fst_ = new StdVectorFst();
    if (model_->hcl_fst_) {
        g_fst_->AddState();
        g_fst_->SetStart(0);
        g_fst_->AddState();
        g_fst_->SetFinal(1, fst::TropicalWeight::One());
        g_fst_->AddArc(1, StdArc(0, 0, fst::TropicalWeight::One(), 0));

        // Create simple word loop FST
        stringstream ss(grammar);
        string token;

        while (getline(ss, token, ' ')) {
            int32 id = model_->word_syms_->Find(token);
            g_fst_->AddArc(0, StdArc(id, id, fst::TropicalWeight::One(), 1));
        }
        ArcSort(g_fst_, ILabelCompare<StdArc>());

        decode_fst_ = LookaheadComposeFst(*model_->hcl_fst_, *g_fst_, model_->disambig_);
    } else {
        decode_fst_ = NULL;
        KALDI_ERR << "Can't create decoding graph";
    }

    decoder_ = new kaldi::SingleUtteranceNnet3Decoder(model_->nnet3_decoding_config_,
            *model_->trans_model_,
            *model_->decodable_info_,
            model_->hclg_fst_ ? *model_->hclg_fst_ : *decode_fst_,
            feature_pipeline_);

    frame_offset_ = 0;
    round_offset_ = 0;
    input_finalized_ = false;
    spk_feature_ = NULL;

    InitRescoring();
}

KaldiRecognizer::KaldiRecognizer(Model *model, SpkModel *spk_model, float sample_frequency) : model_(model), spk_model_(spk_model), sample_frequency_(sample_frequency) {

    model_->Ref();
    spk_model->Ref();

    feature_pipeline_ = new kaldi::OnlineNnet2FeaturePipeline (model_->feature_info_);
    silence_weighting_ = new kaldi::OnlineSilenceWeighting(*model_->trans_model_, model_->feature_info_.silence_weighting_config, 3);

    decode_fst_ = NULL;
    g_fst_ = NULL;

    if (!model_->hclg_fst_) {
        if (model_->hcl_fst_ && model_->g_fst_) {
            decode_fst_ = LookaheadComposeFst(*model_->hcl_fst_, *model_->g_fst_, model_->disambig_);
        } else {
            KALDI_ERR << "Can't create decoding graph";
        }
    }

    decoder_ = new kaldi::SingleUtteranceNnet3Decoder(model_->nnet3_decoding_config_,
            *model_->trans_model_,
            *model_->decodable_info_,
            model_->hclg_fst_ ? *model_->hclg_fst_ : *decode_fst_,
            feature_pipeline_);

    frame_offset_ = 0;
    round_offset_ = 0;
    input_finalized_ = false;

    spk_feature_ = new OnlineMfcc(spk_model_->spkvector_mfcc_opts);

    InitRescoring();
}

KaldiRecognizer::~KaldiRecognizer() {
    delete feature_pipeline_;
    delete silence_weighting_;
    delete decoder_;
    delete g_fst_;
    delete decode_fst_;
    delete spk_feature_;
    delete lm_fst_;

    model_->Unref();
    if (spk_model_)
         spk_model_->Unref();
}

void KaldiRecognizer::InitRescoring()
{
    if (model_->std_lm_fst_) {
        fst::CacheOptions cache_opts(true, 50000);
        fst::MapFstOptions mapfst_opts(cache_opts);
        fst::StdToLatticeMapper<kaldi::BaseFloat> mapper;
        lm_fst_ = new fst::MapFst<fst::StdArc, kaldi::LatticeArc, fst::StdToLatticeMapper<kaldi::BaseFloat> >(*model_->std_lm_fst_, mapper, mapfst_opts);
    } else {
        lm_fst_ = NULL;
    }
}

void KaldiRecognizer::CleanUp()
{
    delete silence_weighting_;
    silence_weighting_ = new kaldi::OnlineSilenceWeighting(*model_->trans_model_, model_->feature_info_.silence_weighting_config, 3);

    if (spk_feature_) {
        delete spk_feature_;
        spk_feature_ = new OnlineMfcc(spk_model_->spkvector_mfcc_opts);
    }

    frame_offset_ += decoder_->NumFramesDecoded();

    // Each 10 minutes we drop the pipeline to save memory in continuous processing
    if (frame_offset_ > 20000) {
        round_offset_ += frame_offset_;
        frame_offset_ = 0;

        delete decoder_;
        delete feature_pipeline_;

        feature_pipeline_ = new kaldi::OnlineNnet2FeaturePipeline (model_->feature_info_);
        decoder_ = new kaldi::SingleUtteranceNnet3Decoder(model_->nnet3_decoding_config_,
            *model_->trans_model_,
            *model_->decodable_info_,
            model_->hclg_fst_ ? *model_->hclg_fst_ : *decode_fst_,
            feature_pipeline_);
    } else {
        decoder_->InitDecoding(frame_offset_);
    }
}

void KaldiRecognizer::UpdateSilenceWeights()
{
    if (silence_weighting_->Active() && feature_pipeline_->NumFramesReady() > 0 &&
        feature_pipeline_->IvectorFeature() != NULL) {
        vector<pair<int32, BaseFloat> > delta_weights;
        silence_weighting_->ComputeCurrentTraceback(decoder_->Decoder());
        silence_weighting_->GetDeltaWeights(feature_pipeline_->NumFramesReady(),
                                          frame_offset_ * 3,
                                          &delta_weights);
        feature_pipeline_->UpdateFrameWeights(delta_weights);
    }
}

bool KaldiRecognizer::AcceptWaveform(const char *data, int len)
{
    Vector<BaseFloat> wave;
    wave.Resize(len / 2, kUndefined);
    for (int i = 0; i < len / 2; i++)
        wave(i) = *(((short *)data) + i);
    return AcceptWaveform(wave);
}

bool KaldiRecognizer::AcceptWaveform(const short *sdata, int len)
{
    Vector<BaseFloat> wave;
    wave.Resize(len, kUndefined);
    for (int i = 0; i < len; i++)
        wave(i) = sdata[i];
    return AcceptWaveform(wave);
}

bool KaldiRecognizer::AcceptWaveform(const float *fdata, int len)
{
    Vector<BaseFloat> wave;
    wave.Resize(len, kUndefined);
    for (int i = 0; i < len; i++)
        wave(i) = fdata[i];
    return AcceptWaveform(wave);
}

bool KaldiRecognizer::AcceptWaveform(Vector<BaseFloat> &wdata)
{
    if (input_finalized_) {
        CleanUp();
        input_finalized_ = false;
    }

    int step = static_cast<int>(sample_frequency_ * 0.2);
    for (int i = 0; i < wdata.Dim(); i+= step) {
        SubVector<BaseFloat> r = wdata.Range(i, std::min(step, wdata.Dim() - i));
        feature_pipeline_->AcceptWaveform(sample_frequency_, r);
        UpdateSilenceWeights();
        decoder_->AdvanceDecoding();
    }

    if (spk_feature_) {
        spk_feature_->AcceptWaveform(sample_frequency_, wdata);
    }

    if (decoder_->EndpointDetected(model_->endpoint_config_)) {
        return true;
    }

    return false;
}

// Computes an xvector from a chunk of speech features.
static void RunNnetComputation(const MatrixBase<BaseFloat> &features,
    const nnet3::Nnet &nnet, nnet3::CachingOptimizingCompiler *compiler,
    Vector<BaseFloat> *xvector) 
{
    nnet3::ComputationRequest request;
    request.need_model_derivative = false;
    request.store_component_stats = false;
    request.inputs.push_back(
    nnet3::IoSpecification("input", 0, features.NumRows()));
    nnet3::IoSpecification output_spec;
    output_spec.name = "output";
    output_spec.has_deriv = false;
    output_spec.indexes.resize(1);
    request.outputs.resize(1);
    request.outputs[0].Swap(&output_spec);
    shared_ptr<const nnet3::NnetComputation> computation = compiler->Compile(request);
    nnet3::Nnet *nnet_to_update = NULL;  // we're not doing any update.
    nnet3::NnetComputer computer(nnet3::NnetComputeOptions(), *computation,
                    nnet, nnet_to_update);
    CuMatrix<BaseFloat> input_feats_cu(features);
    computer.AcceptInput("input", &input_feats_cu);
    computer.Run();
    CuMatrix<BaseFloat> cu_output;
    computer.GetOutputDestructive("output", &cu_output);
    xvector->Resize(cu_output.NumCols());
    xvector->CopyFromVec(cu_output.Row(0));
}

void KaldiRecognizer::GetSpkVector(Vector<BaseFloat> &xvector)
{
    vector<int32> nonsilence_frames;
    if (silence_weighting_->Active() && feature_pipeline_->NumFramesReady() > 0) {
        silence_weighting_->ComputeCurrentTraceback(decoder_->Decoder(), true);
        silence_weighting_->GetNonsilenceFrames(feature_pipeline_->NumFramesReady(),
                                          frame_offset_ * 3,
                                          &nonsilence_frames);
    }

    int num_frames = spk_feature_->NumFramesReady();
    Matrix<BaseFloat> mfcc(num_frames, spk_feature_->Dim());
    for (int i = 0; i < num_frames; ++i) {
       if (std::find(nonsilence_frames.begin(),
                     nonsilence_frames.end(), i % 3) == nonsilence_frames.end())
           continue;
       Vector<BaseFloat> feat(spk_feature_->Dim());
       spk_feature_->GetFrame(i, &feat);
       mfcc.CopyRowFromVec(feat, i);
    }
    SlidingWindowCmnOptions cmvn_opts;
    Matrix<BaseFloat> features(mfcc.NumRows(), mfcc.NumCols(), kUndefined);
    SlidingWindowCmn(cmvn_opts, mfcc, &features);

    nnet3::NnetSimpleComputationOptions opts;
    nnet3::CachingOptimizingCompilerOptions compiler_config;
    nnet3::CachingOptimizingCompiler compiler(spk_model_->speaker_nnet, opts.optimize_config, compiler_config);

    RunNnetComputation(features, spk_model_->speaker_nnet, &compiler, &xvector);
}

const char* KaldiRecognizer::Result()
{

    if (!input_finalized_) {
        decoder_->FinalizeDecoding();
        input_finalized_ = true;
    }

    if (decoder_->NumFramesDecoded() == 0) {
        last_result_ = "{\"text\": \"\"}";
        return last_result_.c_str();
    }

    kaldi::CompactLattice clat;
    decoder_->GetLattice(true, &clat);

    if (model_->std_lm_fst_) {
        Lattice lat1;

        ConvertLattice(clat, &lat1);
        fst::ScaleLattice(fst::GraphLatticeScale(-1.0), &lat1);
        fst::ArcSort(&lat1, fst::OLabelCompare<kaldi::LatticeArc>());
        kaldi::Lattice composed_lat;
        fst::Compose(lat1, *lm_fst_, &composed_lat);
        fst::Invert(&composed_lat);
        kaldi::CompactLattice determinized_lat;
        DeterminizeLattice(composed_lat, &determinized_lat);
        fst::ScaleLattice(fst::GraphLatticeScale(-1), &determinized_lat);
        fst::ArcSort(&determinized_lat, fst::OLabelCompare<kaldi::CompactLatticeArc>());

        kaldi::ConstArpaLmDeterministicFst const_arpa_fst(model_->const_arpa_);
        kaldi::CompactLattice composed_clat;
        kaldi::ComposeCompactLatticeDeterministic(determinized_lat, &const_arpa_fst, &composed_clat);
        kaldi::Lattice composed_lat1;
        ConvertLattice(composed_clat, &composed_lat1);
        fst::Invert(&composed_lat1);
        DeterminizeLattice(composed_lat1, &clat);
    }

    fst::ScaleLattice(fst::LatticeScale(9.0, 10.0), &clat);
    CompactLattice aligned_lat;
    if (model_->winfo_) {
        WordAlignLattice(clat, *model_->trans_model_, *model_->winfo_, 0, &aligned_lat);
    } else {
        aligned_lat = clat;
    }

    MinimumBayesRisk mbr(aligned_lat);
    const vector<BaseFloat> &conf = mbr.GetOneBestConfidences();
    const vector<int32> &words = mbr.GetOneBest();
    const vector<pair<BaseFloat, BaseFloat> > &times =
          mbr.GetOneBestTimes();

    int size = words.size();

    json::JSON obj;
    stringstream text;

    // Create JSON object
    for (int i = 0; i < size; i++) {
        json::JSON word;
        word["word"] = model_->word_syms_->Find(words[i]);
        word["start"] = (round_offset_ + frame_offset_ + times[i].first) * 0.03;
        word["end"] = (round_offset_ + frame_offset_ + times[i].second) * 0.03;
        word["conf"] = conf[i];
        obj["result"].append(word);

        if (i) {
            text << " ";
        }
        text << model_->word_syms_->Find(words[i]);
    }
    obj["text"] = text.str();

    if (spk_model_) {
        Vector<BaseFloat> xvector;
        GetSpkVector(xvector);
        for (int i = 0; i < xvector.Dim(); i++) {
            obj["spk"].append(xvector(i));
        }
    }

    last_result_ = obj.dump();
    return last_result_.c_str();
}

const char* KaldiRecognizer::PartialResult()
{
    json::JSON res;
    if (decoder_->NumFramesDecoded() == 0) {
        res["partial"] = "";
        last_result_ = res.dump();
        return last_result_.c_str();
    }

    kaldi::Lattice lat;
    decoder_->GetBestPath(false, &lat);
    vector<kaldi::int32> alignment, words;
    LatticeWeight weight;
    GetLinearSymbolSequence(lat, &alignment, &words, &weight);

    ostringstream text;
    for (size_t i = 0; i < words.size(); i++) {
        if (i) {
            text << " ";
        }
        text << model_->word_syms_->Find(words[i]);
    }
    res["partial"] = text.str();

    last_result_ = res.dump();
    return last_result_.c_str();
}

const char* KaldiRecognizer::FinalResult()
{
    if (!input_finalized_) {
        feature_pipeline_->InputFinished();
        UpdateSilenceWeights();
        decoder_->AdvanceDecoding();
        decoder_->FinalizeDecoding();
        input_finalized_ = true;
        return Result();
    } else {
        last_result_ = "{\"text\": \"\"}";
        return last_result_.c_str();
    }
}
