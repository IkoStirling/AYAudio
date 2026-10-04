// Included inside namespace workbench after Waveform.
class ChartCanvas final : public LeafWidget {
public:
    chart::Document* document=nullptr; chart::History* history=nullptr;
    Waveform* wave=nullptr; chart::Judge* judge=nullptr;
    std::function<void()> changed;
    bool player=false, dragging=false, panning=false;
    int division=4, lane=0, selected=-1, draggedNote=-1;
    double start=0, finish=0, now=-2, lastX=0;
    float timelineX(double t) const {const auto b=getWorldBounds();return b.minX+float((t-wave->origin)/std::max(.001,wave->visible))*(b.maxX-b.minX);}
    double time(float x)const{const auto b=getWorldBounds();return std::clamp(wave->origin+(x-b.minX)/std::max(1.f,b.maxX-b.minX)*wave->visible,0.0,wave->duration);}
    float laneY(int l)const{const auto b=getWorldBounds();return b.minY+24+(b.maxY-b.minY-24)*l/4;}
    int noteAt(float x,int l)const{for(int i=int(document->notes.size())-1;i>=0;--i){const auto& n=document->notes[i];if(n.lane==l && x>=timelineX(n.time)-6 && x<=timelineX(n.end)+8)return i;}return -1;}
    bool onMouseButtonDown(const UIMouseEvent& e)override{
        if(player || !hitTest(e.mousePos) || wave->duration<=0)return false;
        const auto b=getWorldBounds();lane=std::clamp(int((e.mousePos.y-b.minY-24)/std::max(1.f,(b.maxY-b.minY-24)/4)),0,3);
        const int found=noteAt(e.mousePos.x,lane);
        if(e.mouseButton==1){if(found>=0){history->remember(*document);document->notes.erase(document->notes.begin()+found);selected=-1;if(changed)changed();}markDirty();return true;}
        if(e.mouseButton==2){panning=true;lastX=e.mousePos.x;return true;}
        if(e.mouseButton!=0)return false;
        start=finish=document->snap(time(e.mousePos.x),division);draggedNote=found;selected=found;dragging=true;markDirty();return true;
    }
    bool onMouseMove(const UIMouseEvent& e)override{
        if(panning){wave->origin=std::clamp(wave->origin-(e.mousePos.x-lastX)/std::max(1.f,getSize().x)*wave->visible,0.0,std::max(0.0,wave->duration-wave->visible));lastX=e.mousePos.x;wave->markDirty();markDirty();return true;}
        if(!dragging)return false;finish=document->snap(time(e.mousePos.x),division);markDirty();return true;
    }
    bool onMouseButtonUp(const UIMouseEvent&)override{
        if(panning){panning=false;return true;}if(!dragging)return false;dragging=false;
        if(draggedNote>=0 && std::abs(finish-start)<.000001){if(changed)changed();return true;}
        history->remember(*document);
        if(draggedNote>=0){auto& note=document->notes[draggedNote];const double delta=finish-start;note.end+=delta;note.time+=delta;}
        else {document->notes.push_back({lane,std::min(start,finish),std::max(start,finish)});selected=int(document->notes.size())-1;}
        try{document->validate(wave->duration);}catch(...){history->move(*document,true);history->redo.clear();selected=-1;}
        markDirty();if(changed)changed();return true;
    }
    bool onMouseWheel(const UIMouseWheelEvent& e)override{if(player)return false;const bool result=wave->onMouseWheel(e);markDirty();return result;}
    void onCaptureCancelled()override{dragging=panning=false;markDirty();}
    void onRender(IRenderBackend& r)override{
        const auto b=getWorldBounds();r.drawRect(b,color("$color.bg.surface"));r.pushClip(b);
        const auto accent=color("$color.accent");
        if(player){
            const float width=(b.maxX-b.minX)/4,hit=b.maxY-56,scale=(hit-b.minY-28)/3;
            for(int l=0;l<4;++l){const float x=b.minX+l*width;r.drawRect({x,b.minY,x+width-1,b.maxY},judge->held[l]?FVector4{.12f,.22f,.32f,1}:FVector4{.08f,.09f,.12f,1});r.drawText({x,hit+12,x+width,b.maxY},std::wstring(1,L"DFJK"[l]),20,color("$color.text.primary"));}
            r.drawRect({b.minX,hit,b.maxX,hit+2},color("$color.text.primary"));
            for(size_t i=0;i<document->notes.size();++i){const auto& n=document->notes[i];if(judge->states[i]!=chart::Judge::Pending && judge->states[i]!=chart::Judge::Holding)continue;
                const float bottom=hit-float(n.time-now)*scale,top=hit-float(n.end-now)*scale,x=b.minX+n.lane*width+width*.14f;
                if(bottom<b.minY || top>hit+30)continue;
                if(n.end>n.time)r.drawRect({x+width*.12f,std::max(b.minY,top),x+width*.6f,std::min(hit,bottom)},FVector4{.2f,.4f,.55f,1});
                r.drawRect({x,bottom-5,x+width*.72f,bottom+5},accent);
            }
            if(now<0)r.drawText({b.minX,b.minY+24,b.maxX,b.minY+64},L"准备 · "+std::to_wstring(int(std::ceil(-now))),24,color("$color.text.primary"));
        }else{
            const float h=(b.maxY-b.minY-24)/4;
            for(int l=0;l<4;++l){const float y=laneY(l);r.drawRect({b.minX,y,b.maxX,y+1},color("$color.border"));r.drawText({b.maxX-40,y+6,b.maxX,y+24},std::wstring(1,L"DFJK"[l]),13,color("$color.text.muted"));}
            if(wave->duration>0){double step=60/document->bpm/division;while(step/wave->visible*(b.maxX-b.minX)<8)step*=2;
                double t=document->offset+std::ceil((wave->origin-document->offset)/step)*step;
                for(int i=0;i<1024 && t<=wave->origin+wave->visible;++i,t+=step){const float x=timelineX(t);r.drawRect({x,b.minY+24,x+1,b.maxY},FVector4{.17f,.19f,.24f,1});}
            }
            for(size_t i=0;i<document->notes.size();++i){const auto& n=document->notes[i];const float x=timelineX(n.time),right=timelineX(n.end),y=laneY(n.lane)+h*.28f;
                if(right<b.minX || x>b.maxX)continue;
                r.drawRect({x,y,std::max(x+8,right),y+h*.45f},int(i)==selected?FVector4{.95f,.7f,.2f,1}:accent);
            }
            if(dragging && draggedNote<0){const float x=timelineX(std::min(start,finish)),right=timelineX(std::max(start,finish)),y=laneY(lane)+h*.28f;r.drawRect({x,y,std::max(x+8,right),y+h*.45f},FVector4{.95f,.7f,.2f,1});}
        }
        r.popClip();
    }
};

class ChartDesk {
public:
    AudioEditorSession& session;
    chart::Document document;chart::History history;chart::Judge judge;
    VBox* page=nullptr;Waveform* wave=nullptr;ChartCanvas* canvas=nullptr;
    TextLabel *status=nullptr,*audioName=nullptr;
    TextInput *bpm=nullptr,*offset=nullptr,*path=nullptr,*startInput=nullptr,*endInput=nullptr;
    ComboBox *snap=nullptr,*laneInput=nullptr;
    std::function<AudioEngine*()> engine;
    std::function<std::string()> assets,picker;
    std::function<void(const std::string&)> saved;
    std::function<bool(const std::string&,std::string&)> launch;
    std::function<std::string()> folder;
    AudioSubSystem* audio=nullptr;
    std::shared_ptr<ayt::resource::Audio> music;
    AudioEngine* bound=nullptr;uint64_t generation=0;AudioClipId clip=0;VoiceHandle voice=0;
    std::string source,assetOverride,pinnedRoot,lastFile,lastBytes;
    bool preview=false,running=false,paused=false,countdown=false,dirty=false;
    double clock=0,anchor=0,pausedTime=0;uint64_t clockFrame=0;
    std::chrono::steady_clock::time_point epoch{},frameEpoch{};
    std::wstring message=L"选择音乐后，在轨道中放置音符";
    static std::wstring wide(const std::string& value){return std::filesystem::u8path(value).wstring();}
    static std::string narrow(const std::wstring& value){return chart::utf8(std::filesystem::path(value));}
    static double elapsed(std::chrono::steady_clock::time_point t){return std::chrono::duration<double>(std::chrono::steady_clock::now()-t).count();}
    std::string root()const{const auto current=assets?assets():std::string{};return current.empty()?assetOverride:current;}
    bool valid()const{auto* e=engine();return e && e==bound && e->lifecycleGeneration()==generation;}
    ChartDesk(AudioEditorSession& owner,VBox* parent,bool player,std::function<AudioEngine*()> getEngine,
              AudioSubSystem* subsystem,std::function<std::string()> getAssets,std::function<std::string()> choose,
              std::function<void(const std::string&)> notify,std::function<bool(const std::string&,std::string&)> runner,
              std::function<std::string()> chooseFolder):session(owner),page(parent),engine(std::move(getEngine)),assets(std::move(getAssets)),picker(std::move(choose)),saved(std::move(notify)),launch(std::move(runner)),folder(std::move(chooseFolder)),audio(subsystem),preview(player){
        audioName=label(L"未选择音乐","chart_music_name");page->addWidget(audioName,28);
        if(!preview){
            auto* r=row(page,34);button(r,L"选择音乐…","chart_music_open",[this]{if(picker){const auto p=picker();if(!p.empty())loadMusic(p);}},110);
            r->addWidget(label(L"BPM"),38);bpm=field(r,"chart_bpm",L"120",76);r->addWidget(label(L"节拍起点 / ms"),100);offset=field(r,"chart_offset",L"0",82);
            r->addWidget(label(L"吸附"),36);snap=new ComboBox();snap->setId("chart_snap");snap->setItems({L"1 拍",L"1/2 拍",L"1/4 拍",L"1/8 拍",L"1/16 拍"});snap->setSelectedIndex(2);r->addWidget(snap,95);
            auto commit=[this]{try{const double b=parse(bpm->getText()),o=parse(offset->getText())/1000;if(b<20 || b>1000 || std::abs(o)>600)throw std::runtime_error("settings");document.bpm=b;document.offset=o;dirty=true;}catch(...){bpm->setText(number(document.bpm));offset->setText(number(document.offset*1000));}canvas->markDirty();};
            bpm->setOnSubmit([commit](const std::wstring&){commit();});bpm->setOnFocusLostNotify(commit);offset->setOnSubmit([commit](const std::wstring&){commit();});offset->setOnFocusLostNotify(commit);
            auto* file=row(page,36);file->addWidget(label(L"谱面虚拟路径"),105);
            const auto stamp=std::chrono::system_clock::now().time_since_epoch().count();path=field(file,"chart_path",L"audio/charts/Chart_"+std::to_wstring(stamp)+L".chart.json",0);
            button(file,L"打开谱面","chart_load",[this]{loadAsset();},90);button(file,L"保存谱面","chart_save",[this]{saveAsset();},90);
            button(file,L"资产目录…","chart_folder",[this]{if(folder){const auto chosen=folder();if(!chosen.empty())assetOverride=chosen;}},90);
        }
        wave=new Waveform();wave->setId("chart_waveform");wave->editableRange=false;wave->showSelection=false;wave->emptyText=L"音乐波形 · 滚轮缩放，中键拖动轨道平移";wave->onSeek=[this](double t){wave->position=t;wave->markDirty();};
        if(!preview)page->addWidget(wave,96);
        auto* transport=row(page,36);
        if(preview){button(transport,L"重新开始","chart_restart",[this]{start();},110);button(transport,L"暂停 / 继续","chart_pause",[this]{togglePause();},120);}
        else{button(transport,L"运行谱面","chart_run",[this]{runSnapshot();},110);button(transport,L"试听音乐","chart_listen",[this]{listen();},100);button(transport,L"停止试听","chart_listen_stop",[this]{stop();},100);
            button(transport,L"撤销","chart_undo",[this]{if(history.move(document,true)){dirty=true;canvas->selected=-1;canvas->markDirty();}},70);button(transport,L"重做","chart_redo",[this]{if(history.move(document,false)){dirty=true;canvas->selected=-1;canvas->markDirty();}},70);}
        canvas=new ChartCanvas();canvas->setId(preview?"chart_player":"chart_editor");canvas->document=&document;canvas->history=&history;canvas->judge=&judge;canvas->wave=wave;canvas->player=preview;
        canvas->changed=[this]{dirty=true;const int selected=canvas->selected;if(selected>=0 && selected<int(document.notes.size())){const auto& n=document.notes[selected];laneInput->setSelectedIndex(n.lane);startInput->setText(number(n.time,3));endInput->setText(number(n.end,3));}};
        page->addWidget(canvas,preview?570:260);
        if(!preview){
            page->addWidget(label(L"单击空白放置点按 · 左键拖出长按 · 拖动已有音符移动 · 右键删除 · 中键平移"),24);
            auto* edit=row(page,36);laneInput=new ComboBox();laneInput->setItems({L"D",L"F",L"J",L"K"});laneInput->setSelectedIndex(0);edit->addWidget(laneInput,65);
            edit->addWidget(label(L"起点 / s"),68);startInput=field(edit,"chart_note_start",L"0",95);edit->addWidget(label(L"终点 / s"),68);endInput=field(edit,"chart_note_end",L"0",95);
            button(edit,L"添加 / 应用","chart_note_apply",[this]{applyNote();},110);
        }else page->addWidget(label(L"四轨 D F J K · 点按与长按 · Esc 暂停 · 失焦自动暂停"),28);
        status=label(message,"chart_status");page->addWidget(status,38);
    }
    ~ChartDesk(){release();}
    TextInput* field(HBox* r,const char* id,const std::wstring& value,float width){auto* input=new TextInput();input->setId(id);input->setText(value);r->addWidget(input,width);return input;}
    void release(){stop();if(valid() && clip)bound->releaseClip(clip);clip=0;bound=nullptr;music.reset();}
    void stop(){if(valid() && voice)bound->stop(voice);voice=0;running=paused=countdown=false;judge.held.fill(false);}
    bool loadMusic(const std::string& file,const chart::Document* expected=nullptr){
        try{ayt::resource::AudioLoader loader;auto next=std::dynamic_pointer_cast<ayt::resource::Audio>(loader.load(file));
            if(!next || !next->getChannels() || !next->getSampleRate() || (next->getBitsPerSample()!=16 && next->getBitsPerSample()!=32))throw std::runtime_error("audio decode failed");
            const double nextDuration=double(next->getDataSize())/(next->getBitsPerSample()/8)/next->getChannels()/next->getSampleRate();
            if(nextDuration<=0)throw std::runtime_error("audio is empty");if(expected)expected->validate(nextDuration);
            auto* e=engine();const auto nextClip=e&&audio?audio->registerClipFromResource(next.get()):0;if(e && !nextClip)throw std::runtime_error("audio registration failed");
            release();music=next;clip=nextClip;bound=e;generation=e?e->lifecycleGeneration():0;source=file;
            document.audio=chart::utf8(std::filesystem::absolute(std::filesystem::path(file)));document.audioRelative=false;
            const auto sr=music->getSampleRate();const auto channels=music->getChannels();const size_t bytes=music->getBitsPerSample()/8,frames=music->getDataSize()/bytes/channels;
            wave->duration=double(frames)/sr;wave->origin=0;wave->visible=std::min(12.0,wave->duration);wave->position=0;wave->peaks.assign(2048,0);
            for(size_t f=0;f<frames;++f)for(size_t c=0;c<channels;++c){const auto* ptr=music->getData()+(f*channels+c)*bytes;float sample=0;if(bytes==4)std::memcpy(&sample,ptr,4);else{int16_t v;std::memcpy(&v,ptr,2);sample=v/32768.f;}if(std::isfinite(sample)){auto& peak=wave->peaks[std::min<size_t>(2047,f*2048/frames)];peak=std::max(peak,std::abs(sample));}}
            pinnedRoot=root();dirty=true;audioName->setText(std::filesystem::path(file).filename().wstring()+L" · "+number(wave->duration)+L" s");message=L"音乐已载入 · 四轨 D F J K";canvas->markDirty();wave->markDirty();return true;
        }catch(const std::exception& ex){message=L"音乐载入失败："+wide(ex.what());return false;}
    }
    void applyNote(){try{chart::Note note{laneInput->getSelectedIndex(),parse(startInput->getText()),parse(endInput->getText())};auto next=document;const int index=canvas->selected;if(index>=0 && index<int(next.notes.size()))next.notes[index]=note;else next.notes.push_back(note);next.validate(wave->duration);history.remember(document);document=std::move(next);dirty=true;canvas->selected=-1;message=L"音符已更新";canvas->markDirty();}catch(...){message=L"音符无效：起点 ≥ 0，终点 ≥ 起点，且同轨音符不能重叠";}}
    void checkReady(){document.validate(wave->duration);if(!music || !valid() || !clip)throw std::runtime_error("select music and initialize audio first");}
    chart::Document stored()const{auto result=document;if(!root().empty()){const auto base=std::filesystem::weakly_canonical(std::filesystem::u8path(root())),file=std::filesystem::weakly_canonical(std::filesystem::path(source));const auto rel=file.lexically_relative(base);if(!rel.empty() && *rel.begin()!=".."){result.audio=narrow(rel.generic_wstring());result.audioRelative=true;}}return result;}
    void saveAsset(){try{
        document.validate(wave->duration);if(!pinnedRoot.empty() && root()!=pinnedRoot)throw std::runtime_error("project changed; return to the original project");
        const auto logical=narrow(path->getText());if(!logical.ends_with(".chart.json"))throw std::runtime_error("use .chart.json extension");
        const auto file=chart::containedPath(root(),logical);if(std::filesystem::exists(file) && (chart::utf8(file)!=lastFile || chart::read(file)!=lastBytes))throw std::runtime_error("file exists or changed externally; use another path");
        const auto bytes=stored().json().dump(2);std::filesystem::create_directories(file.parent_path());if(!ayt::io::File::atomicWrite(file.string(),bytes.data(),bytes.size()))throw std::runtime_error("cannot write chart");
        lastFile=chart::utf8(file);lastBytes=bytes;pinnedRoot=root();dirty=false;message=L"谱面已保存："+path->getText();if(saved)saved(logical);
    }catch(const std::exception& ex){message=L"保存失败："+wide(ex.what());}}
    bool loadFile(const std::filesystem::path& file){try{const auto bytes=chart::read(file);auto next=chart::Document::fromJson(nlohmann::json::parse(bytes));
        const auto audioFile=next.audioRelative?chart::containedPath(root(),next.audio):std::filesystem::u8path(next.audio);
        if(!loadMusic(audioFile.string(),&next))return false;document=std::move(next);history={};judge.reset(document);canvas->selected=-1;lastFile=chart::utf8(file);lastBytes=bytes;dirty=false;
        if(bpm){bpm->setText(number(document.bpm));offset->setText(number(document.offset*1000));}message=L"谱面已载入";canvas->markDirty();return true;
    }catch(const std::exception& ex){message=L"谱面载入失败："+wide(ex.what());return false;}}
    void loadAsset(){try{const auto logical=narrow(path->getText());if(!logical.ends_with(".chart.json"))throw std::runtime_error("use .chart.json extension");loadFile(chart::containedPath(root(),logical));}catch(const std::exception& ex){message=L"谱面载入失败："+wide(ex.what());}}
    void runSnapshot(){try{checkReady();stop();auto snapshot=document;snapshot.audio=chart::utf8(std::filesystem::absolute(std::filesystem::path(source)));snapshot.audioRelative=false;
        const auto temp=std::filesystem::temp_directory_path()/"AYAudioChartPreview";std::filesystem::create_directories(temp);
        static std::atomic<uint64_t> serial{0};const auto file=temp/("Preview_"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count())+"_"+std::to_string(++serial)+".chart.json");
        std::ofstream out(file,std::ios::binary|std::ios::trunc);out<<snapshot.json().dump(2);out.close();if(!out)throw std::runtime_error("cannot create preview snapshot");std::string error;
        if(!launch || !launch(chart::utf8(file),error))throw std::runtime_error(error.empty()?"preview executable unavailable":error);
        message=L"独立试玩已启动 · 编辑内容保留";
    }catch(const std::exception& ex){message=L"运行失败："+wide(ex.what());}}
    void listen(){try{checkReady();stop();voice=bound->play(clip,AudioBus::Music);if(voice)bound->setVoicePositionFrames(voice,uint64_t(wave->position*bound->outputSampleRate()));message=L"正在试听音乐";}catch(const std::exception& ex){message=L"试听失败："+wide(ex.what());}}
    void start(){try{checkReady();stop();judge.reset(document);epoch=std::chrono::steady_clock::now();clock=canvas->now=-2;running=countdown=true;message=L"准备 · D F J K";}catch(const std::exception& ex){message=L"运行失败："+wide(ex.what());}}
    double time(){
        if(paused)return pausedTime;if(countdown)return -2+elapsed(epoch);
        if(!running || !valid() || !voice)return clock;
        VoiceRuntime state;if(bound->voiceState(voice,state)){
            if(state.positionFrames!=clockFrame){clockFrame=state.positionFrames;anchor=double(clockFrame)/bound->outputSampleRate();frameEpoch=std::chrono::steady_clock::now();}
            clock=std::max(clock,std::min(wave->duration,anchor+std::min(.05,elapsed(frameEpoch))));
        }else clock=wave->duration;
        return clock;
    }
    void togglePause(){if(!running)return;if(!paused){pausedTime=time();paused=true;judge.held.fill(false);for(size_t i=0;i<judge.states.size();++i)if(judge.states[i]==chart::Judge::Holding)judge.finish(i,chart::Judge::Miss);if(valid() && voice)bound->setVoicePaused(voice,true);message=L"已暂停 · 点击继续或按 Esc";}
        else{paused=false;if(countdown)epoch=std::chrono::steady_clock::now()-std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(pausedTime+2));else{anchor=pausedTime;frameEpoch=std::chrono::steady_clock::now();if(valid() && voice)bound->setVoicePaused(voice,false);}message=L"试玩继续";}}
    bool key(int key,bool down){if(!preview)return false;if(key==27){if(down)togglePause();return true;}const std::string keys="DFJK";const auto index=keys.find(char(key));if(index==std::string::npos)return false;if(running && !paused && !countdown){judge.key(document,int(index),down,time());canvas->markDirty();}return true;}
    void focusLost(){if(preview && running && !paused)togglePause();canvas->onCaptureCancelled();}
    void tick(){
        if(!preview){canvas->division=1<<snap->getSelectedIndex();if(valid() && voice){VoiceRuntime state;if(bound->voiceState(voice,state)){wave->position=double(state.positionFrames)/bound->outputSampleRate();wave->markDirty();}else voice=0;}
            status->setText((dirty?L"未保存 · ":L"")+message+L" · 音符 "+std::to_wstring(document.notes.size()));canvas->markDirty();return;}
        if(running && !valid()){stop();message=L"音频设备已关闭，请重新载入音乐";}
        if(running && !paused){double t=time();if(countdown && t>=0){countdown=false;voice=bound->play(clip,AudioBus::Music);if(!voice){stop();message=L"音乐启动失败";}else{clock=anchor=0;clockFrame=0;frameEpoch=std::chrono::steady_clock::now();t=0;}}
            if(running){canvas->now=t;judge.update(document,t);canvas->markDirty();if(!countdown && t>=wave->duration){stop();message=L"试玩完成";}}
        }
        status->setText(message+L" · Perfect "+std::to_wstring(judge.count(chart::Judge::Perfect))+L" / Good "+std::to_wstring(judge.count(chart::Judge::Good))+L" / Miss "+std::to_wstring(judge.count(chart::Judge::Miss))+L" · Combo "+std::to_wstring(judge.combo)+L" · 误差 "+number(judge.lastError*1000,1)+L" ms");
    }
};
