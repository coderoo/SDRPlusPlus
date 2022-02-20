#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <signal_path/sink.h>
#include <dsp/audio.h>
#include <dsp/processing.h>
#include <spdlog/spdlog.h>
#include <RtAudio.h>
#include <config.h>
#include <options.h>
#include <unordered_set>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "audio_sink",
    /* Description:     */ "Audio sink module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 2
};

class AudioSink : SinkManager::Sink {
public:
    std::shared_ptr<ConfigManager> config;
    AudioSink(SinkManager::Stream* stream, std::string streamName, std::shared_ptr<ConfigManager> config) {
        this->config = config;
        _stream = stream;
        _streamName = streamName;
        s2m.init(_stream->sinkOut);
        monoPacker.init(&s2m.out, 512);
        stereoPacker.init(_stream->sinkOut, 512);

        bool created = false;
        std::string device = "";
        config->acquire();
        if (!config->conf.contains(_streamName)) {
            created = true;
            config->conf[_streamName]["device"] = "";
            config->conf[_streamName]["devices"] = json({});
        }
        device = config->conf[_streamName]["device"];
        config->release(created);

        int count = audio.getDeviceCount();
        RtAudio::DeviceInfo info;
        for (int i = 0; i < count; i++) {
            info = audio.getDeviceInfo(i);
            if (!info.probed) { continue; }
            if (info.outputChannels == 0) { continue; }
            if (info.isDefaultOutput) { defaultDevId = devList.size(); }
            devList.push_back(info);
            txtDevList += info.name;
            txtDevList += '\0';
            auto ni = info;
            ni.name += " -> left";
            devList.push_back(ni);
            txtDevList += ni.name;
            txtDevList += '\0';
            ni = info;
            ni.name += " -> right";
            devList.push_back(ni);
            txtDevList += ni.name;
            txtDevList += '\0';
            deviceIds.push_back(i);
            deviceIds.push_back(i);
            deviceIds.push_back(i);
        }

        selectByName(device);
    }

    ~AudioSink() {
    }

    void start() {
        if (running) {
            return;
        }
        doStart();
        running = true;
    }

    void stop() {
        if (!running) {
            return;
        }
        doStop();
        running = false;
    }

    void selectFirst() {
        selectById(defaultDevId);
    }

    void selectByName(std::string name) {
        for (int i = 0; i < devList.size(); i++) {
            if (devList[i].name == name) {
                selectById(i);
                return;
            }
        }
        selectFirst();
    }

    void selectById(int id) {
        devId = id;
        bool created = false;
        config->acquire();
        if (!config->conf[_streamName]["devices"].contains(devList[id].name)) {
            created = true;
            config->conf[_streamName]["devices"][devList[id].name] = devList[id].preferredSampleRate;
        }
        sampleRate = config->conf[_streamName]["devices"][devList[id].name];
        config->release(created);

        sampleRates = devList[id].sampleRates;
        sampleRatesTxt = "";
        char buf[256];
        bool found = false;
        unsigned int defaultId = 0;
        unsigned int defaultSr = devList[id].preferredSampleRate;
        for (int i = 0; i < sampleRates.size(); i++) {
            if (sampleRates[i] == sampleRate) {
                found = true;
                srId = i;
            }
            if (sampleRates[i] == defaultSr) {
                defaultId = i;
            }
            sprintf(buf, "%d", sampleRates[i]);
            sampleRatesTxt += buf;
            sampleRatesTxt += '\0';
        }
        if (!found) {
            sampleRate = defaultSr;
            srId = defaultId;
        }

        _stream->setSampleRate(sampleRate);

        if (running) { doStop(); }
        if (running) { doStart(); }
    }

    void menuHandler() {
        float menuWidth = ImGui::GetContentRegionAvailWidth();

        ImGui::SetNextItemWidth(menuWidth);
        if (ImGui::Combo(("##_audio_sink_dev_" + _streamName).c_str(), &devId, txtDevList.c_str())) {
            selectById(devId);
            config->acquire();
            config->conf[_streamName]["device"] = devList[devId].name;
            config->release(true);
        }

        if (!SinkManager::isSecondaryStream(_streamName)) {
            ImGui::SetNextItemWidth(menuWidth);
            if (ImGui::Combo(("##_audio_sink_sr_" + _streamName).c_str(), &srId, sampleRatesTxt.c_str())) {
                sampleRate = sampleRates[srId];
                _stream->setSampleRate(sampleRate);
                if (running) {
                    doStop();
                    doStart();
                }
                config->acquire();
                config->conf[_streamName]["devices"][devList[devId].name] = sampleRate;
                config->release(true);
            }
        }
    }

private:
    void doStart() {
        RtAudio::StreamParameters parameters;
        parameters.deviceId = deviceIds[devId];
        parameters.nChannels = 2;
        unsigned int bufferFrames = sampleRate / 60;
        RtAudio::StreamOptions opts;
        opts.flags = RTAUDIO_MINIMIZE_LATENCY;
        opts.streamName = _streamName;

        try {
            audio.openStream(&parameters, NULL, RTAUDIO_FLOAT32, sampleRate, &bufferFrames, &callback, this, &opts);
            stereoPacker.setSampleCount(bufferFrames);
            audio.startStream();
            stereoPacker.start();
        }
        catch (RtAudioError& e) {
            spdlog::error("Could not open audio device");
            return;
        }

        spdlog::info("RtAudio stream open");
    }

    void doStop() {
        s2m.stop();
        monoPacker.stop();
        stereoPacker.stop();
        monoPacker.out.stopReader();
        stereoPacker.out.stopReader();
        audio.stopStream();
        audio.closeStream();
        monoPacker.out.clearReadStop();
        stereoPacker.out.clearReadStop();
    }

    static int callback(void* outputBuffer, void* inputBuffer, unsigned int nBufferFrames, double streamTime, RtAudioStreamStatus status, void* userData) {
        AudioSink* _this = (AudioSink*)userData;
        int count = _this->stereoPacker.out.read();
        if (count < 0) { return 0; }



        // For debug purposes only...
        // if (nBufferFrames != count) { spdlog::warn("Buffer size mismatch, wanted {0}, was asked for {1}", count, nBufferFrames); }
        // for (int i = 0; i < count; i++) {
        //     if (_this->stereoPacker.out.readBuf[i].l == NAN || _this->stereoPacker.out.readBuf[i].r == NAN) { spdlog::error("NAN in audio data"); }
        //     if (_this->stereoPacker.out.readBuf[i].l == INFINITY || _this->stereoPacker.out.readBuf[i].r == INFINITY) { spdlog::error("INFINITY in audio data"); }
        //     if (_this->stereoPacker.out.readBuf[i].l == -INFINITY || _this->stereoPacker.out.readBuf[i].r == -INFINITY) { spdlog::error("-INFINITY in audio data"); }
        // }

        memcpy(outputBuffer, _this->stereoPacker.out.readBuf, nBufferFrames * sizeof(dsp::stereo_t));
        auto channel = _this->devId % 3; // 0=stereo 1=left 2=right
        auto stereoOut = (dsp::stereo_t *)outputBuffer;
        switch(channel) {
            default:
                break;
            case 1:
                for(int i=0; i<nBufferFrames; i++) {
                    stereoOut[i].r = 0;
                }
                break;
            case 2:
                for(int i=0; i<nBufferFrames; i++) {
                    stereoOut[i].l = 0;
                }
                break;
        }
        _this->stereoPacker.out.flush();
        return 0;
    }

    SinkManager::Stream* _stream;
    dsp::StereoToMono s2m;
    dsp::Packer<float> monoPacker;
    dsp::Packer<dsp::stereo_t> stereoPacker;

    std::string _streamName;

    int srId = 0;
    int devCount;
    int devId = 0;
    bool running = false;

    unsigned int defaultDevId = 0;

    std::vector<RtAudio::DeviceInfo> devList;
    std::vector<unsigned int> deviceIds;
    std::string txtDevList;

    std::vector<unsigned int> sampleRates;
    std::string sampleRatesTxt;
    unsigned int sampleRate = 48000;

    RtAudio audio;
};

class AudioSinkModule : public ModuleManager::Instance {
public:
    int index;
    std::shared_ptr<ConfigManager> config;
    AudioSinkModule(std::string name, int index, std::shared_ptr<ConfigManager> config) {
        this->name = name;
        this->index = index;
        this->config = config;
        provider.create = create_sink;
        provider.ctx = this;

        sigpath::sinkManager.registerSinkProvider("Audio"+getSuffix(), provider);
    }

    ~AudioSinkModule() {
        // Unregister sink, this will automatically stop and delete all instances of the audio sink
        sigpath::sinkManager.unregisterSinkProvider("Audio"+getSuffix());
    }

    std::string getSuffix() {
        if (this->index > 0) {
            return std::to_string(this->index);
        } else {
            return "";
        }
    };

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    static SinkManager::Sink* create_sink(SinkManager::Stream* stream, std::string streamName, void* ctx) {
        auto module = (AudioSinkModule *)ctx;
        return (SinkManager::Sink*)(new AudioSink(stream, streamName, module->config));
    }

    std::string name;
    bool enabled = true;
    SinkManager::SinkProvider provider;
};

MOD_EXPORT void _INIT_() {
}

static std::unordered_set<int> countInstances;

MOD_EXPORT void* _CREATE_INSTANCE_(std::string name) {
    int foundIndex = -1;
    for(int i=0; i<10; i++) {
        if (countInstances.find(i) == countInstances.end()) {
            foundIndex = i;
            break;
        }
    }
    countInstances.insert(foundIndex);

    auto config = std::make_shared<ConfigManager>();
    json def = json({});
    if (foundIndex > 0) {
        config->setPath(options::opts.root + "/audio_sink_config"+std::to_string(foundIndex)+".json");
    } else {
        config->setPath(options::opts.root + "/audio_sink_config.json");
    }
    config->load(def);
    config->enableAutoSave();
    AudioSinkModule* instance = new AudioSinkModule(name, foundIndex, config);
    return instance;
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    auto inst = (AudioSinkModule*)instance;
    inst->config->disableAutoSave();
    inst->config->save();
    countInstances.erase(inst->index);
    delete inst;
}

MOD_EXPORT void _END_() {
}