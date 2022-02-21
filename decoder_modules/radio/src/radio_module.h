#pragma once
#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <config.h>
#include <dsp/chain.h>
#include <dsp/noise_reduction.h>
#include <dsp/logmmse_nr.h>
#include <core.h>
#include <utils/optionlist.h>
#include "radio_interface.h"
#include "demod.h"

ConfigManager config;

#define CONCAT(a, b) ((std::string(a) + b).c_str())

std::map<DeemphasisMode, double> deempTaus = {
    { DEEMP_MODE_22US, 22e-6 },
    { DEEMP_MODE_50US, 50e-6 },
    { DEEMP_MODE_75US, 75e-6 }
};

std::map<IFNRPreset, double> ifnrTaps = {
    { IFNR_PRESET_NOAA_APT, 9},
    { IFNR_PRESET_VOICE, 15 },
    { IFNR_PRESET_NARROW_BAND, 31 },
    { IFNR_PRESET_BROADCAST, 32 }
};

class RadioModule : public ModuleManager::Instance {
public:
    RadioModule(std::string name) {
        this->name = name;

        // Intialize option lists
        deempModes.define("None", DEEMP_MODE_NONE);
        deempModes.define("22us", DEEMP_MODE_22US);
        deempModes.define("50us", DEEMP_MODE_50US);
        deempModes.define("75us", DEEMP_MODE_75US);

        ifnrPresets.define("NOAA APT", IFNR_PRESET_NOAA_APT);
        ifnrPresets.define("Voice", IFNR_PRESET_VOICE);
        ifnrPresets.define("Narrow Band", IFNR_PRESET_NARROW_BAND);

        // Initialize the config if it doesn't exist
        bool created = false;
        config.acquire();
        if (!config.conf.contains(name)) {
            config.conf[name]["selectedDemodId"] = 1;
            created = true;
        }
        selectedDemodID = config.conf[name]["selectedDemodId"];
        config.release(created);

        // Create demodulator instances
        demods.fill(NULL);
        demods[RADIO_DEMOD_WFM] = new demod::WFM();
        demods[RADIO_DEMOD_NFM] = new demod::NFM();
        demods[RADIO_DEMOD_AM] = new demod::AM();
        demods[RADIO_DEMOD_USB] = new demod::USB();
        demods[RADIO_DEMOD_LSB] = new demod::LSB();
        demods[RADIO_DEMOD_DSB] = new demod::DSB();
        demods[RADIO_DEMOD_CW] = new demod::CW();
        demods[RADIO_DEMOD_RAW] = new demod::RAW();

        // Initialize the VFO
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, 200000, 200000, 50000, 200000, false);
        onUserChangedBandwidthHandler.handler = vfoUserChangedBandwidthHandler;
        onUserChangedBandwidthHandler.ctx = this;
        vfo->wtfVFO->onUserChangedBandwidth.bindHandler(&onUserChangedBandwidthHandler);


        // Initialize IF DSP chain
        ifChainOutputChanged.ctx = this;
        ifChainOutputChanged.handler = ifChainOutputChangeHandler;
        ifChain.init(vfo->output, &ifChainOutputChanged);

        fmnr.block.init(NULL, 32);
        notch.block.init(NULL, 0.5, 0, 250000); // TODO: The rate has to depend on IF sample rate so the width is always the same
        ifavg.block.init(NULL);
        squelch.block.init(NULL, MIN_SQUELCH);
        lmmsenr.block.init(NULL);
        nb.block.init(NULL, -100.0f);

        ifChain.add(&lmmsenr);
        ifChain.add(&notch);
        ifChain.add(&ifavg);
        ifChain.add(&squelch);
        ifChain.add(&fmnr);
        ifChain.add(&nb);

        // Load configuration for and enabled all demodulators
        EventHandler<dsp::stream<dsp::stereo_t>*> _demodOutputChangeHandler;
        EventHandler<float> _demodAfbwChangedHandler;
        _demodOutputChangeHandler.handler = demodOutputChangeHandler;
        _demodOutputChangeHandler.ctx = this;
        _demodAfbwChangedHandler.handler = demodAfbwChangedHandler;
        _demodAfbwChangedHandler.ctx = this;
        for (auto& demod : demods) {
            if (!demod) { continue; }

            // Default config
            double bw = demod->getDefaultBandwidth();
            if (!config.conf[name].contains(demod->getName())) {
                config.conf[name][demod->getName()]["bandwidth"] = bw;
                if (auto hasAgc = dynamic_cast<demod::HasAGC*>(demod); hasAgc) {
                    config.conf[name][demod->getName()]["agcFallRate"] = hasAgc->getAGC().getFallRate();
                }
                config.conf[name][demod->getName()]["snapInterval"] = demod->getDefaultSnapInterval();
                config.conf[name][demod->getName()]["squelchLevel"] = MIN_SQUELCH;
                config.conf[name][demod->getName()]["squelchEnabled"] = false;
            }
            bw = std::clamp<double>(bw, demod->getMinBandwidth(), demod->getMaxBandwidth());

            // Initialize
            demod->init(name, &config, ifChain.getOutput(), bw, _demodOutputChangeHandler, _demodAfbwChangedHandler, audioSampleRate);
        }

        // Initialize audio DSP chain
        afChainOutputChanged.ctx = this;
        afChainOutputChanged.handler = afChainOutputChangeHandler;
        afChain.init(&dummyAudioStream, &afChainOutputChanged);

        win.init(24000, 24000, 48000);
        resamp.block.init(NULL, &win, 250000, 48000);
        deemp.block.init(NULL, 48000, 50e-6);
        deemp.block.bypass = false;

        afChain.add(&resamp);
        afChain.add(&deemp);

        // Initialize the sink
        srChangeHandler.ctx = this;
        srChangeHandler.handler = sampleRateChangeHandler;

        afsplitter.init(afChain.getOutput());

        streams.emplace_back(std::make_shared<SinkManager::Stream>());
        streamNames.emplace_back(name);
        afsplitter.bindStream(streams.back()->getInput());
        streams.back()->init(&srChangeHandler, audioSampleRate);
        sigpath::sinkManager.registerStream(name, &*streams.front());
        sigpath::sinkManager.onAddSubstream.bindHandler(&onAddSubstreamHandler);
        sigpath::sinkManager.onRemoveSubstream.bindHandler(&onRemoveSubstreamHandler);
        onAddSubstreamHandler.handler = &addSubstreamHandler;
        onAddSubstreamHandler.ctx = this;
        onRemoveSubstreamHandler.handler = &removeSubstreamHandler;
        onRemoveSubstreamHandler.ctx = this;

        for(int i=1; i<10; i++) {
            auto secondaryName = name + SinkManager::secondaryStreamSuffix(i);
            if (sigpath::sinkManager.configContains(secondaryName)) {
                addSecondaryStream(secondaryName);
            }
        }


        // Select the demodulator
        selectDemodByID((DemodID)selectedDemodID);

        // Start IF chain
        ifChain.start();

        // Start AF chain
        afChain.start();

        afsplitter.start();

        // Start streams, the rest was started when selecting the demodulator
        for(auto &s: streams) {
            s->start();
        }

        // Register the menu
        gui::menu.registerEntry(name, menuHandler, this, this);

        // Register the module interface
        core::modComManager.registerInterface("radio", name, moduleInterfaceHandler, this);
    }

    std::shared_ptr<SinkManager::Stream> addSecondaryStream(std::string secondaryName = "") {
        if (secondaryName.empty()) {
            secondaryName = name + SinkManager::secondaryStreamSuffix(streams.size());
        }
        streams.emplace_back(std::make_shared<SinkManager::Stream>());
        afsplitter.bindStream(streams.back()->getInput());
        streams.back()->init(&srChangeHandler, audioSampleRate);
        sigpath::sinkManager.registerStream(secondaryName, &*streams.back());
        streamNames.emplace_back(secondaryName);

        return streams.back();

    }

    ~RadioModule() {
        gui::menu.removeEntry(name);
        afsplitter.stop();
        for(auto &s: streams) {
            s->stop();
        }
        if (enabled) {
            disable();
        }
        for(int i=0; i<streams.size(); i++) {
            sigpath::sinkManager.unregisterStream(name + SinkManager::secondaryStreamSuffix(i));
        }
        sigpath::sinkManager.onAddSubstream.unbindHandler(&onAddSubstreamHandler);
        sigpath::sinkManager.onRemoveSubstream.unbindHandler(&onRemoveSubstreamHandler);
    }

    void postInit() {}

    void enable() {
        enabled = true;
        if (!vfo) {
            vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, 200000, 200000, 50000, 200000, false);
            vfo->wtfVFO->onUserChangedBandwidth.bindHandler(&onUserChangedBandwidthHandler);
        }
        ifChain.setInput(vfo->output);
        ifChain.start();
        afsplitter.start();
        selectDemodByID((DemodID)selectedDemodID);
        afChain.start();
    }

    void disable() {
        enabled = false;
        ifChain.stop();
        afsplitter.stop();
        if (selectedDemod) { selectedDemod->stop(); }
        afChain.stop();
        if (vfo) { sigpath::vfoManager.deleteVFO(vfo); }
        vfo = NULL;
    }

    bool isEnabled() {
        return enabled;
    }

    std::string name;

    enum DemodID {
        RADIO_DEMOD_NFM,
        RADIO_DEMOD_WFM,
        RADIO_DEMOD_AM,
        RADIO_DEMOD_DSB,
        RADIO_DEMOD_USB,
        RADIO_DEMOD_CW,
        RADIO_DEMOD_LSB,
        RADIO_DEMOD_RAW,
        _RADIO_DEMOD_COUNT,
    };

private:
    static void menuHandler(void* ctx) {
        RadioModule* _this = (RadioModule*)ctx;

        if (!_this->enabled) { style::beginDisabled(); }

        float menuWidth = ImGui::GetContentRegionAvailWidth();
        ImGui::BeginGroup();

        ImGui::Columns(4, CONCAT("RadioModeColumns##_", _this->name), false);
        if (ImGui::RadioButton(CONCAT("NFM##_", _this->name), _this->selectedDemodID == 0) && _this->selectedDemodID != 0) {
            _this->selectDemodByID(RADIO_DEMOD_NFM);
        }
        if (ImGui::RadioButton(CONCAT("WFM##_", _this->name), _this->selectedDemodID == 1) && _this->selectedDemodID != 1) {
            _this->selectDemodByID(RADIO_DEMOD_WFM);
        }
        ImGui::NextColumn();
        if (ImGui::RadioButton(CONCAT("AM##_", _this->name), _this->selectedDemodID == 2) && _this->selectedDemodID != 2) {
            _this->selectDemodByID(RADIO_DEMOD_AM);
        }
        if (ImGui::RadioButton(CONCAT("DSB##_", _this->name), _this->selectedDemodID == 3) && _this->selectedDemodID != 3) {
            _this->selectDemodByID(RADIO_DEMOD_DSB);
        }
        ImGui::NextColumn();
        if (ImGui::RadioButton(CONCAT("USB##_", _this->name), _this->selectedDemodID == 4) && _this->selectedDemodID != 4) {
            _this->selectDemodByID(RADIO_DEMOD_USB);
        }
        if (ImGui::RadioButton(CONCAT("CW##_", _this->name), _this->selectedDemodID == 5) && _this->selectedDemodID != 5) {
            _this->selectDemodByID(RADIO_DEMOD_CW);
        };
        ImGui::NextColumn();
        if (ImGui::RadioButton(CONCAT("LSB##_", _this->name), _this->selectedDemodID == 6) && _this->selectedDemodID != 6) {
            _this->selectDemodByID(RADIO_DEMOD_LSB);
        }
        if (ImGui::RadioButton(CONCAT("RAW##_", _this->name), _this->selectedDemodID == 7) && _this->selectedDemodID != 7) {
            _this->selectDemodByID(RADIO_DEMOD_RAW);
        };
        ImGui::Columns(1, CONCAT("EndRadioModeColumns##_", _this->name), false);

        ImGui::EndGroup();

        if (!_this->bandwidthLocked) {
            ImGui::LeftLabel("Bandwidth");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputFloat(("##_radio_bw_" + _this->name).c_str(), &_this->bandwidth, 1, 100, "%.0f")) {
                _this->bandwidth = std::clamp<float>(_this->bandwidth, _this->minBandwidth, _this->maxBandwidth);
                _this->setBandwidth(_this->bandwidth);
            }
        }

        if (auto hasAgc = dynamic_cast<demod::HasAGC*>(_this->selectedDemod); hasAgc) {
            ImGui::LeftLabel("AGC Speed");
            if (_this->agcFallRate  < 0) {
                _this->agcFallRate = hasAgc->getAGC().getFallRate();
            }
            if (ImGui::SliderFloat(("##_radio_agc_fallrate_" + _this->name).c_str(), &_this->agcFallRate, 0.1, 30, "%.1f")) {
                hasAgc->getAGC().setFallRate(_this->agcFallRate);
                config.acquire();
                config.conf[_this->name][_this->selectedDemod->getName()]["agcFallRate"] = _this->agcFallRate;
                config.release(true);
            }
        } else {
            _this->agcFallRate = -1;
        }

        // VFO snap interval
        ImGui::LeftLabel("Snap Interval");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(("##_radio_snap_" + _this->name).c_str(), &_this->snapInterval, 1, 100)) {
            if (_this->snapInterval < 1) { _this->snapInterval = 1; }
            _this->vfo->setSnapInterval(_this->snapInterval);
            config.acquire();
            config.conf[_this->name][_this->selectedDemod->getName()]["snapInterval"] = _this->snapInterval;
            config.release(true);
        }

        // Deemphasis mode
        if (_this->deempAllowed) {
            ImGui::LeftLabel("De-emphasis");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::Combo(("##_radio_wfm_deemp_" + _this->name).c_str(), &_this->deempId, _this->deempModes.txt)) {
                _this->setDeemphasisMode(_this->deempModes[_this->deempId]);
            }
        }

        if (ImGui::Checkbox(("AF NR##_radio_logmmse_nr_" + _this->name).c_str(), &_this->logmmseNrEnabled)) {
            _this->setLogMMSEBandwidth(_this->logmmseBandwidth * 1000);
            _this->setLogMMSEEnabled(_this->logmmseNrEnabled);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("This is LOGMMSE algorithm which is run over the audio frequency.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::SliderInt(("##_radio_logmmse_wf" + _this->name).c_str(), &_this->logmmseBandwidth, 1, 24, "%d KHz")) {
            _this->setLogMMSEBandwidth(_this->logmmseBandwidth * 1000);
        }

//        if (ImGui::Checkbox(("IF AVG##_radio_if_avg_" + _this->name).c_str(), &_this->ifavgEnabled)) {
//            _this->setIFAVGEnabled(_this->ifavgEnabled);
//        }
//        ImGui::SameLine();
//        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
//        if (ImGui::SliderInt(("##_radio_logmmse_wf" + _this->name).c_str(), &_this->logmmseFreq, 8, 192, "%d KHz")) {
//            _this->setLogMMSEFrequency(_this->logmmseFreq * 1000);
//        }

        // Squelch
        if (ImGui::Checkbox(("Squelch##_radio_sqelch_ena_" + _this->name).c_str(), &_this->squelchEnabled)) {
            _this->setSquelchEnabled(_this->squelchEnabled);
        }
        if (!_this->squelchEnabled && _this->enabled) { style::beginDisabled(); }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::SliderFloat(("##_radio_sqelch_lvl_" + _this->name).c_str(), &_this->squelchLevel, _this->MIN_SQUELCH, _this->MAX_SQUELCH, "%.3fdB")) {
            _this->setSquelchLevel(_this->squelchLevel);
        }
        if (!_this->squelchEnabled && _this->enabled) { style::endDisabled(); }

        // Noise blanker
        if (_this->nbAllowed) {
            if (ImGui::Checkbox(("Noise Blanker##_radio_nb_ena_" + _this->name).c_str(), &_this->nbEnabled)) {
                _this->setNoiseBlankerEnabled(_this->nbEnabled);
            }
            if (!_this->nbEnabled && _this->enabled) { style::beginDisabled(); }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::SliderFloat(("##_radio_nb_lvl_" + _this->name).c_str(), &_this->nbLevel, 0.0f, -100.0f, "%.3fdB")) {
                _this->setNoiseBlankerLevel(_this->nbLevel);
            }
            if (!_this->nbEnabled && _this->enabled) { style::endDisabled(); }
        }


        // // Notch filter
        // if (ImGui::Checkbox("Notch##_radio_notch_ena_", &_this->notchEnabled)) {
        //     _this->ifChain.setState(&_this->notch, _this->notchEnabled);
        // }
        // if (ImGui::SliderFloat(("NF##_radio_notch_freq_" + _this->name).c_str(), &_this->notchPos, -7500, 7500)) {
        //     _this->notch.block.setOffset(_this->notchPos);
        // }
        // if (ImGui::SliderFloat(("NW##_radio_notch_width_" + _this->name).c_str(), &_this->notchWidth, 0, 1000)) {
        //     // TODO: Implement
        // }

        // FM IF Noise Reduction
        if (_this->FMIFNRAllowed) {
            if (ImGui::Checkbox(("IF Noise Reduction##_radio_fmifnr_ena_" + _this->name).c_str(), &_this->FMIFNREnabled)) {
                _this->setFMIFNREnabled(_this->FMIFNREnabled);
            }
            if (_this->selectedDemodID == RADIO_DEMOD_NFM) {
                if (!_this->FMIFNREnabled && _this->enabled) { style::beginDisabled(); }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
                if (ImGui::Combo(("##_radio_fmifnr_ena_" + _this->name).c_str(), &_this->fmIFPresetId, _this->ifnrPresets.txt)) {
                    _this->setIFNRPreset(_this->ifnrPresets[_this->fmIFPresetId]);
                }
                if (!_this->FMIFNREnabled && _this->enabled) { style::endDisabled(); }
            }
        }

        // Demodulator specific menu
        _this->selectedDemod->showMenu();

        if (!_this->enabled) { style::endDisabled(); }
    }

    void selectDemodByID(DemodID id) {
        demod::Demodulator* demod = demods[id];
        if (!demod) {
            spdlog::error("Demodulator {0} not implemented", id);
            return;
        }
        selectedDemodID = id;
        selectDemod(demod);

        // Save config
        config.acquire();
        config.conf[name]["selectedDemodId"] = id;
        config.release(true);
    }

    void selectDemod(demod::Demodulator* demod) {
        // Stopcurrently selected demodulator and select new
        if (selectedDemod) { selectedDemod->stop(); }
        selectedDemod = demod;

        // Give the demodulator the most recent audio SR
        selectedDemod->AFSampRateChanged(audioSampleRate);

        // Set the demodulator's input
        selectedDemod->setInput(ifChain.getOutput());

        // Set AF chain's input
        afChain.setInput(selectedDemod->getOutput());

        // Load config
        bandwidth = selectedDemod->getDefaultBandwidth();
        minBandwidth = selectedDemod->getMinBandwidth();
        maxBandwidth = selectedDemod->getMaxBandwidth();
        bandwidthLocked = selectedDemod->getBandwidthLocked();
        snapInterval = selectedDemod->getDefaultSnapInterval();
        squelchLevel = MIN_SQUELCH;
        deempAllowed = selectedDemod->getDeempAllowed();
        deempId = deempModes.valueId((DeemphasisMode)selectedDemod->getDefaultDeemphasisMode());
        squelchEnabled = false;
        postProcEnabled = selectedDemod->getPostProcEnabled();
        FMIFNRAllowed = selectedDemod->getFMIFNRAllowed();
        FMIFNREnabled = false;
        fmIFPresetId = ifnrPresets.valueId(IFNR_PRESET_VOICE);
        nbAllowed = selectedDemod->getNBAllowed();
        nbEnabled = false;
        nbLevel = 0.0f;
        config.acquire();
        if (config.conf[name][selectedDemod->getName()].contains("bandwidth")) {
            bandwidth = config.conf[name][selectedDemod->getName()]["bandwidth"];
            bandwidth = std::clamp<double>(bandwidth, minBandwidth, maxBandwidth);
        }
        if (config.conf[name][selectedDemod->getName()].contains("agcFallRate")) {
            agcFallRate = config.conf[name][selectedDemod->getName()]["agcFallRate"];
            agcFallRate = std::clamp<double>(bandwidth, 0, 30);
        }
        if (config.conf[name][selectedDemod->getName()].contains("snapInterval")) {
            snapInterval = config.conf[name][selectedDemod->getName()]["snapInterval"];
        }
        if (config.conf[name][selectedDemod->getName()].contains("squelchLevel")) {
            squelchLevel = config.conf[name][selectedDemod->getName()]["squelchLevel"];
        }
        if (config.conf[name][selectedDemod->getName()].contains("agcFallRate")) {
            agcFallRate = config.conf[name][selectedDemod->getName()]["agcFallRate"]; if (agcFallRate == 0) agcFallRate = -1;
        }
        if (config.conf[name][selectedDemod->getName()].contains("squelchEnabled")) {
            squelchEnabled = config.conf[name][selectedDemod->getName()]["squelchEnabled"];
        }
        if (config.conf[name][selectedDemod->getName()].contains("deempMode")) {
            if (!config.conf[name][selectedDemod->getName()]["deempMode"].is_string()) {
                config.conf[name][selectedDemod->getName()]["deempMode"] = deempModes.key(deempId);
            }

            std::string deempOpt = config.conf[name][selectedDemod->getName()]["deempMode"];
            if (deempModes.keyExists(deempOpt)) {
                deempId = deempModes.keyId(deempOpt);
            }
        }
        if (config.conf[name][selectedDemod->getName()].contains("logmmseNrEnabled")) {
            logmmseNrEnabled = config.conf[name][selectedDemod->getName()]["logmmseNrEnabled"];
        }
        if (config.conf[name][selectedDemod->getName()].contains("FMIFNREnabled")) {
            FMIFNREnabled = config.conf[name][selectedDemod->getName()]["FMIFNREnabled"];
        }
        if (config.conf[name][selectedDemod->getName()].contains("fmifnrPreset")) {
            std::string presetOpt = config.conf[name][selectedDemod->getName()]["fmifnrPreset"];
            if (ifnrPresets.keyExists(presetOpt)) {
                fmIFPresetId = ifnrPresets.keyId(presetOpt);
            }
        }
        if (config.conf[name][selectedDemod->getName()].contains("noiseBlankerEnabled")) {
            nbEnabled = config.conf[name][selectedDemod->getName()]["noiseBlankerEnabled"];
        }
        if (config.conf[name][selectedDemod->getName()].contains("noiseBlankerEnabled")) {
            nbEnabled = config.conf[name][selectedDemod->getName()]["noiseBlankerEnabled"];
        }
        if (config.conf[name][selectedDemod->getName()].contains("noiseBlankerLevel")) {
            nbLevel = config.conf[name][selectedDemod->getName()]["noiseBlankerLevel"];
        }
        config.release();

        // Configure VFO
        if (vfo) {
            vfo->setBandwidthLimits(minBandwidth, maxBandwidth, selectedDemod->getBandwidthLocked());
            vfo->setReference(selectedDemod->getVFOReference());
            vfo->setSnapInterval(snapInterval);
            vfo->setSampleRate(selectedDemod->getIFSampleRate(), bandwidth);
        }

        // Configure bandwidth
        setBandwidth(bandwidth);

        // Configure FM IF Noise Reduction
        setIFNRPreset((selectedDemodID == RADIO_DEMOD_NFM) ? ifnrPresets[fmIFPresetId] : IFNR_PRESET_BROADCAST);
        setFMIFNREnabled(FMIFNRAllowed ? FMIFNREnabled : false);

        // Configure notch
        notch.block.setSampleRate(selectedDemod->getIFSampleRate());

        // Configure squelch
        squelch.block.setLevel(squelchLevel);
        setSquelchEnabled(squelchEnabled);

        // noise reduction
        setLogMMSEEnabled(logmmseNrEnabled);

        // Configure noise blanker
        nb.block.setLevel(nbLevel);
        setNoiseBlankerEnabled(nbEnabled);

        // Configure AF chain
        if (postProcEnabled) {
            // Configure resampler
            afChain.stop();
            resamp.block.setInSampleRate(selectedDemod->getAFSampleRate());
            setAudioSampleRate(audioSampleRate);
            afChain.enable(&resamp);

            // Configure deemphasis
            setDeemphasisMode(deempModes[deempId]);
        }
        else {
            // Disable everyting if post processing is disabled
            afChain.disableAll();
        }

        // Start new demodulator
        selectedDemod->start();
    }


    void setBandwidth(double bw) {
        bw = std::clamp<double>(bw, minBandwidth, maxBandwidth);
        bandwidth = bw;
        if (!selectedDemod) { return; }
        float audioBW = std::min<float>(selectedDemod->getMaxAFBandwidth(), selectedDemod->getAFBandwidth(bandwidth));
        audioBW = std::min<float>(audioBW, audioSampleRate / 2.0);
        vfo->setBandwidth(bandwidth);
        selectedDemod->setBandwidth(bandwidth);

        // Only bother with setting the resampling setting if we're actually post processing and dynamic bw is enabled
        if (selectedDemod->getDynamicAFBandwidth() && postProcEnabled) {
            win.setCutoff(audioBW);
            win.setTransWidth(audioBW);
            resamp.block.updateWindow(&win);
        }

        config.acquire();
        config.conf[name][selectedDemod->getName()]["bandwidth"] = bandwidth;
        config.release(true);
    }

    void setAudioSampleRate(double sr) {
        audioSampleRate = sr;
        if (!selectedDemod) { return; }
        selectedDemod->AFSampRateChanged(audioSampleRate);
        if (!postProcEnabled) {
            // If postproc is disabled, IF SR = AF SR
            minBandwidth = selectedDemod->getMinBandwidth();
            maxBandwidth = selectedDemod->getMaxBandwidth();
            bandwidth = selectedDemod->getIFSampleRate();
            vfo->setBandwidthLimits(minBandwidth, maxBandwidth, selectedDemod->getBandwidthLocked());
            vfo->setSampleRate(selectedDemod->getIFSampleRate(), bandwidth);
            return;
        }
        float audioBW = std::min<float>(selectedDemod->getMaxAFBandwidth(), selectedDemod->getAFBandwidth(bandwidth));
        audioBW = std::min<float>(audioBW, audioSampleRate / 2.0);

        afChain.stop();

        // Configure resampler
        resamp.block.setOutSampleRate(audioSampleRate);
        win.setSampleRate(selectedDemod->getAFSampleRate() * resamp.block.getInterpolation());
        win.setCutoff(audioBW);
        win.setTransWidth(audioBW);
        resamp.block.updateWindow(&win);

        // Configure deemphasis sample rate
        deemp.block.setSampleRate(audioSampleRate);

        afChain.start();
    }

    void setDeemphasisMode(DeemphasisMode mode) {
        deempId = deempModes.valueId(mode);
        if (!postProcEnabled || !selectedDemod) { return; }
        bool deempEnabled = (mode != DEEMP_MODE_NONE);
        if (deempEnabled) { deemp.block.setTau(deempTaus[mode]); }
        afChain.setState(&deemp, deempEnabled);

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["deempMode"] = deempModes.key(deempId);
        config.release(true);
    }

    void setLogMMSEEnabled(bool enable) {
        logmmseNrEnabled = enable;
        if (!selectedDemod) { return; }
        ifChain.setState(&lmmsenr, logmmseNrEnabled);

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["logmmseNrEnabled"] = logmmseNrEnabled;
        config.release(true);
    }

    void setLogMMSEBandwidth(int bandwidthHz) {
        lmmsenr.block.setBandwidth(bandwidthHz);
    }

    void setIFAVGEnabled(bool enabled) {
        ifChain.setState(&ifavg, enabled);
    }

    void setSquelchEnabled(bool enable) {
        squelchEnabled = enable;
        if (!selectedDemod) { return; }
        ifChain.setState(&squelch, squelchEnabled);

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["squelchEnabled"] = squelchEnabled;
        config.release(true);
    }

    void setSquelchLevel(float level) {
        squelchLevel = std::clamp<float>(level, MIN_SQUELCH, MAX_SQUELCH);
        squelch.block.setLevel(squelchLevel);

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["squelchLevel"] = squelchLevel;
        config.release(true);
    }

    void setFMIFNREnabled(bool enabled) {
        FMIFNREnabled = enabled;
        if (!selectedDemod) { return; }
        ifChain.setState(&fmnr, FMIFNREnabled);

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["FMIFNREnabled"] = FMIFNREnabled;
        config.release(true);
    }

    void setNoiseBlankerEnabled(bool enabled) {
        nbEnabled = enabled;
        if (!selectedDemod) { return; }
        ifChain.setState(&nb, nbEnabled);

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["noiseBlankerEnabled"] = nbEnabled;
        config.release(true);
    }

    void setNoiseBlankerLevel(float level) {
        nbLevel = level;
        if (!selectedDemod) { return; }
        nb.block.setLevel(nbLevel);

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["noiseBlankerLevel"] = nbLevel;
        config.release(true);
    }

    void setIFNRPreset(IFNRPreset preset) {
        // Don't save if in broadcast mode
        if (preset == IFNR_PRESET_BROADCAST) {
            if (!selectedDemod) { return; }
            fmnr.block.setTapCount(ifnrTaps[preset]);
            return;
        }

        fmIFPresetId = ifnrPresets.valueId(preset);
        if (!selectedDemod) { return; }
        fmnr.block.setTapCount(ifnrTaps[preset]);

        // Save config
        config.acquire();
        config.conf[name][selectedDemod->getName()]["fmifnrPreset"] = ifnrPresets.key(fmIFPresetId);
        config.release(true);
    }

    static void addSubstreamHandler(std::string name, void* ctx) {
        RadioModule* _this = (RadioModule*)ctx;
        if (name == _this->name) {
            auto stream = _this->addSecondaryStream();
            if (_this->enabled) {
                stream->start();
            }
        }
    }
    static void removeSubstreamHandler(std::string name, void* ctx) {
        RadioModule* _this = (RadioModule*)ctx;
        auto pos = std::find(_this->streamNames.begin(), _this->streamNames.end(), name);
        if (pos == _this->streamNames.end() || pos == _this->streamNames.begin()) // disable first stream removal as well
            return;
        auto index = pos - _this->streamNames.begin();
        auto stream = _this->streams[index];
        if (_this->enabled) {
            stream->stop();
        }
        _this->afsplitter.unbindStream(stream->getInput());
//        stream->init(&srChangeHandler, audioSampleRate);
        _this->streams.erase(_this->streams.begin()+index);
        _this->streamNames.erase(_this->streamNames.begin()+index);
        sigpath::sinkManager.unregisterStream(name);
        core::configManager.acquire();
        auto &streamz = core::configManager.conf["streams"];
        auto iter = streamz.find(name);
        if (iter != streamz.end()) {
            streamz.erase(iter);
        }
        core::configManager.release();
    }

    static void vfoUserChangedBandwidthHandler(double newBw, void* ctx) {
        RadioModule* _this = (RadioModule*)ctx;
        _this->setBandwidth(newBw);
    }

    static void sampleRateChangeHandler(float sampleRate, void* ctx) {
        RadioModule* _this = (RadioModule*)ctx;
        _this->setAudioSampleRate(sampleRate);
    }

    static void demodOutputChangeHandler(dsp::stream<dsp::stereo_t>* output, void* ctx) {
        RadioModule* _this = (RadioModule*)ctx;
        _this->afChain.setInput(output);
    }

    static void demodAfbwChangedHandler(float output, void* ctx) {
        RadioModule* _this = (RadioModule*)ctx;
        
        float audioBW = std::min<float>(_this->selectedDemod->getMaxAFBandwidth(), _this->selectedDemod->getAFBandwidth(_this->bandwidth));
        audioBW = std::min<float>(audioBW, _this->audioSampleRate / 2.0);

        _this->win.setCutoff(audioBW);
        _this->win.setTransWidth(audioBW);
        _this->resamp.block.updateWindow(&_this->win);
    }

    static void ifChainOutputChangeHandler(dsp::stream<dsp::complex_t>* output, void* ctx) {
        RadioModule* _this = (RadioModule*)ctx;
        if (!_this->selectedDemod) { return; }
        _this->selectedDemod->setInput(output);
    }

    static void afChainOutputChangeHandler(dsp::stream<dsp::stereo_t>* output, void* ctx) {
        RadioModule* _this = (RadioModule*)ctx;
        _this->afsplitter.setInput(output);
    }

    static void moduleInterfaceHandler(int code, void* in, void* out, void* ctx) {
        RadioModule* _this = (RadioModule*)ctx;
        if (!_this->enabled || !_this->selectedDemod) { return; }

        // Execute commands
        if (code == RADIO_IFACE_CMD_GET_MODE && out) {
            int* _out = (int*)out;
            *_out = _this->selectedDemodID;
        }
        else if (code == RADIO_IFACE_CMD_SET_MODE && in) {
            int* _in = (int*)in;
            _this->selectDemodByID((DemodID)*_in);
        }
        else if (code == RADIO_IFACE_CMD_GET_BANDWIDTH && out) {
            float* _out = (float*)out;
            *_out = _this->bandwidth;
        }
        else if (code == RADIO_IFACE_CMD_SET_BANDWIDTH && in) {
            float* _in = (float*)in;
            if (_this->bandwidthLocked) { return; }
            _this->setBandwidth(*_in);
        }
        else if (code == RADIO_IFACE_CMD_GET_SQUELCH_ENABLED && out) {
            bool* _out = (bool*)out;
            *_out = _this->squelchEnabled;
        }
        else if (code == RADIO_IFACE_CMD_SET_SQUELCH_ENABLED && in) {
            bool* _in = (bool*)in;
            _this->setSquelchEnabled(*_in);
        }
        else if (code == RADIO_IFACE_CMD_GET_SQUELCH_LEVEL && out) {
            float* _out = (float*)out;
            *_out = _this->squelchLevel;
        }
        else if (code == RADIO_IFACE_CMD_SET_SQUELCH_LEVEL && in) {
            float* _in = (float*)in;
            _this->setSquelchLevel(*_in);
        }
        else {
            return;
        }

        // Success
        return;
    }

    // Handlers
    EventHandler<double> onUserChangedBandwidthHandler;
    EventHandler<float> srChangeHandler;
    EventHandler<std::string> onAddSubstreamHandler;
    EventHandler<std::string> onRemoveSubstreamHandler;
    EventHandler<dsp::stream<dsp::complex_t>*> ifChainOutputChanged;
    EventHandler<dsp::stream<dsp::stereo_t>*> afChainOutputChanged;

    VFOManager::VFO* vfo = NULL;

    // IF chain
    dsp::Chain<dsp::complex_t> ifChain;
    dsp::ChainLink<dsp::FMIFNoiseReduction, dsp::complex_t> fmnr;
    dsp::ChainLink<dsp::LogMMSENoiseReduction, dsp::complex_t> lmmsenr;
    dsp::ChainLink<dsp::NotchFilter, dsp::complex_t> notch;
    dsp::ChainLink<dsp::IFAVGFilter, dsp::complex_t> ifavg;
    dsp::ChainLink<dsp::Squelch, dsp::complex_t> squelch;
    dsp::ChainLink<dsp::NoiseBlanker, dsp::complex_t> nb;

    // Audio chain
    dsp::stream<dsp::stereo_t> dummyAudioStream;
    dsp::Chain<dsp::stereo_t> afChain;
    dsp::filter_window::BlackmanWindow win;
    dsp::ChainLink<dsp::PolyphaseResampler<dsp::stereo_t>, dsp::stereo_t> resamp;
    dsp::ChainLink<dsp::BFMDeemp, dsp::stereo_t> deemp;
    dsp::Splitter<dsp::stereo_t> afsplitter;

    std::vector<std::shared_ptr<SinkManager::Stream>> streams;
    std::vector<std::string> streamNames;

    std::array<demod::Demodulator*, _RADIO_DEMOD_COUNT> demods;
    demod::Demodulator* selectedDemod = NULL;

    OptionList<std::string, DeemphasisMode> deempModes;
    OptionList<std::string, IFNRPreset> ifnrPresets;

    double audioSampleRate = 48000.0;
    float minBandwidth;
    float maxBandwidth;
    float bandwidth;
    float agcFallRate = -1;
    bool bandwidthLocked;
    int snapInterval;
    int selectedDemodID = 1;
    bool postProcEnabled;

    bool squelchEnabled = false;
    float squelchLevel;
    int logmmseBandwidth = 10;

    int deempMode = DEEMP_MODE_NONE;
    int deempId = 0;
    bool deempAllowed;

    bool logmmseNrEnabled = false;
    bool ifavgEnabled = false;

    bool FMIFNRAllowed;
    bool FMIFNREnabled = false;
    int fmIFPresetId;

    bool notchEnabled = false;
    float notchPos = 0;
    float notchWidth = 500;

    bool nbAllowed;
    bool nbEnabled;
    float nbLevel = -100.0f;

    const double MIN_SQUELCH = -100.0;
    const double MAX_SQUELCH = 0.0;

    bool enabled = true;
};
