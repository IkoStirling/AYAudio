// Included inside namespace workbench after Waveform.
class ChartCanvas final : public LeafWidget {
public:
    chart::Document* document=nullptr;chart::History* history=nullptr;
    Waveform* wave=nullptr;chart::Judge* judge=nullptr;
    std::function<void()> changed;
    std::function<void(double)> seek;
    bool player=false,dragging=false,panning=false;
    int division=4,lane=0,selected=-1,draggedNote=-1;
    double start=0,finish=0,now=-.5,lastX=0;
    float timelineX(double t)const{const auto b=getWorldBounds();return b.minX+float((t-wave->origin)/std::max(.001,wave->visible))*(b.maxX-b.minX);}
    double time(float x)const{const auto b=getWorldBounds();return std::clamp(wave->origin+(x-b.minX)/std::max(1.f,b.maxX-b.minX)*wave->visible,0.0,wave->duration);}
    float laneY(int l)const{const auto b=getWorldBounds();return b.minY+24+(b.maxY-b.minY-24)*l/4;}
    int noteAt(float x,int l)const{for(int i=int(document->notes.size())-1;i>=0;--i){const auto& n=document->notes[i];if(n.lane==l&&x>=timelineX(n.time)-6&&x<=timelineX(n.end)+8)return i;}return -1;}
    bool onMouseButtonDown(const UIMouseEvent& e)override{
        if(player||!hitTest(e.mousePos)||wave->duration<=0)return false;
        const auto b=getWorldBounds();
        if(e.mouseButton==2){panning=true;lastX=e.mousePos.x;return true;}
        if(e.mouseButton==0&&e.mousePos.y<b.minY+24){if(seek)seek(time(e.mousePos.x));return true;}
        lane=std::clamp(int((e.mousePos.y-b.minY-24)/std::max(1.f,(b.maxY-b.minY-24)/4)),0,3);
        const int found=noteAt(e.mousePos.x,lane);
        if(e.mouseButton==1){if(found>=0){history->remember(*document);document->notes.erase(document->notes.begin()+found);selected=-1;if(changed)changed();}markDirty();return true;}
        if(e.mouseButton!=0)return false;
        start=finish=document->snap(time(e.mousePos.x),division);draggedNote=found;selected=found;dragging=true;markDirty();return true;
    }
    bool onMouseMove(const UIMouseEvent& e)override{
        if(panning){wave->origin=std::clamp(wave->origin-(e.mousePos.x-lastX)/std::max(1.f,getSize().x)*wave->visible,0.0,std::max(0.0,wave->duration-wave->visible));lastX=e.mousePos.x;wave->markDirty();markDirty();return true;}
        if(!dragging)return false;finish=document->snap(time(e.mousePos.x),division);markDirty();return true;
    }
    bool onMouseButtonUp(const UIMouseEvent&)override{
        if(panning){panning=false;return true;}if(!dragging)return false;dragging=false;
        if(draggedNote>=0&&std::abs(finish-start)<.000001){if(changed)changed();return true;}
        history->remember(*document);
        if(draggedNote>=0){auto& note=document->notes[draggedNote];const double delta=finish-start;note.end+=delta;note.time+=delta;}
        else{document->notes.push_back({lane,std::min(start,finish),std::max(start,finish)});selected=int(document->notes.size())-1;}
        try{document->validate(wave->duration);}catch(...){history->move(*document,true);history->redo.clear();selected=-1;}
        markDirty();if(changed)changed();return true;
    }
    bool onMouseWheel(const UIMouseWheelEvent& e)override{if(player)return false;const bool result=wave->onMouseWheel(e);markDirty();return result;}
    void onCaptureCancelled()override{dragging=panning=false;markDirty();}
    static std::wstring beatLabel(const chart::Document& d,double t){
        const double beat=(t-d.offset)*d.bpm/60;const int whole=int(std::floor(beat+1e-6));
        if(whole<0)return L"前奏";
        return std::to_wstring(whole/4+1)+L":"+std::to_wstring(whole%4+1);
    }
    static void drawGrid(IRenderBackend& r,const chart::Document& d,const FVector4& muted,
                         const FVector4& border,const ayt::math::FRectangle& b,double origin,double visible,int division){
        const double beat=60/d.bpm;int stride=1;const double pxBeat=beat/visible*(b.maxX-b.minX);
        while(pxBeat*stride<70)stride*=2;
        const long long first=static_cast<long long>(std::floor((origin-d.offset)/beat/stride))*stride;
        for(long long index=first,count=0;count<1024;++count,index+=stride){const double t=d.offset+index*beat;
            if(t>origin+visible)break;if(t<origin)continue;
            const float x=b.minX+float((t-origin)/visible)*(b.maxX-b.minX);
            r.drawRect({x,b.minY+24,x+(index%4==0?2.f:1.f),b.maxY},index%4==0?border:FVector4{.17f,.19f,.24f,1});
            r.drawText({x+3,b.minY+2,std::min(x+115,b.maxX),b.minY+22},beatLabel(d,t)+L" · "+number(t,2)+L"s",11,muted);
        }
        const double sub=beat/std::max(1,division);
        if(sub/visible*(b.maxX-b.minX)>=8){double t=d.offset+std::ceil((origin-d.offset)/sub)*sub;
            for(int count=0;count<2048&&t<=origin+visible;++count,t+=sub){const float x=b.minX+float((t-origin)/visible)*(b.maxX-b.minX);r.drawRect({x,b.minY+24,x+1,b.maxY},FVector4{.14f,.16f,.2f,1});}}
    }
    void onRender(IRenderBackend& r)override{
        const auto b=getWorldBounds();r.drawRect(b,color("$color.bg.surface"));r.pushClip(b);
        const auto accent=color("$color.accent");const float h=(b.maxY-b.minY-24)/4;
        if(player){
            const float hit=b.minX+76,scale=std::max(1.f,b.maxX-hit)/6;
            for(int l=0;l<4;++l){const float y=laneY(l);r.drawRect({b.minX,y,b.maxX,y+h-1},judge->held[l]?FVector4{.12f,.22f,.32f,1}:FVector4{.08f,.09f,.12f,1});
                r.drawText({b.minX+10,y+h*.3f,hit-10,y+h*.7f},std::wstring(1,L"DFJK"[l]),20,color("$color.text.primary"));}
            r.drawRect({hit,b.minY+24,hit+2,b.maxY},color("$color.text.primary"));
            const double period=60/document->bpm;double t=document->offset+std::ceil(now/period-document->offset/period)*period;
            for(int i=0;i<256&&t<=now+6;++i,t+=period){const float x=hit+float(t-now)*scale;r.drawRect({x,b.minY+24,x+1,b.maxY},color("$color.border"));r.drawText({x+3,b.minY+2,std::min(x+85,b.maxX),b.minY+22},beatLabel(*document,t),11,color("$color.text.muted"));}
            // Long holds that began earlier remain visible; ordinary pending notes use a sorted window.
            auto drawNote=[&](size_t i){const auto& n=document->notes[i];const auto state=judge->states[i];if(state!=chart::Judge::Pending&&state!=chart::Judge::Holding)return;
                const float x=hit+float(n.time-now)*scale,right=hit+float(n.end-now)*scale,y=laneY(n.lane)+h*.28f;
                if(right<hit-20||x>b.maxX)return;
                if(n.end>n.time)r.drawRect({std::max(hit,x),y+h*.12f,std::min(b.maxX,right),y+h*.33f},FVector4{.2f,.4f,.55f,1});
                r.drawRect({std::max(hit-5,x-5),y,std::max(hit+5,x+5),y+h*.45f},state==chart::Judge::Holding?FVector4{.95f,.7f,.2f,1}:accent);};
            auto begin=std::lower_bound(judge->order.begin(),judge->order.end(),now-.2,[&](size_t i,double v){return document->notes[i].time<v;});
            for(auto it=begin;it!=judge->order.end()&&document->notes[*it].time<=now+6;++it)drawNote(*it);
            for(int i:judge->holding)if(i>=0&&document->notes[i].time<now-.2)drawNote(size_t(i));
            for(auto it=judge->feedback.rbegin();it!=judge->feedback.rend();++it){if(now-it->time>1.4)continue;const auto& n=document->notes[it->note];
                // One visible response per lane; full recent responses are kept below the canvas.
                bool newer=false;for(auto p=judge->feedback.rbegin();p!=it;++p)if(document->notes[p->note].lane==n.lane){newer=true;break;}if(newer)continue;
                const std::wstring grade=it->result==chart::Judge::Perfect?L"Perfect":it->result==chart::Judge::Good?L"Good":L"Miss";
                r.drawText({hit+12,laneY(n.lane)+h*.58f,hit+285,laneY(n.lane)+h*.9f},grade+L" · "+number(it->accuracy,1)+L"% · "+number(it->error*1000,1)+L"ms",14,it->result==chart::Judge::Miss?FVector4{.95f,.35f,.3f,1}:FVector4{.5f,.9f,.65f,1});}
            if(now<0)r.drawText({hit+15,b.minY+32,b.maxX,b.minY+65},L"准备 · "+number(-now,1)+L"s",20,color("$color.text.primary"));
        }else{
            drawGrid(r,*document,color("$color.text.muted"),color("$color.border"),b,wave->origin,std::max(.001,wave->visible),division);
            for(int l=0;l<4;++l){const float y=laneY(l);r.drawRect({b.minX,y,b.maxX,y+1},color("$color.border"));r.drawText({b.maxX-40,y+6,b.maxX,y+24},std::wstring(1,L"DFJK"[l]),13,color("$color.text.muted"));}
            for(size_t i=0;i<document->notes.size();++i){const auto& n=document->notes[i];const float x=timelineX(n.time),right=timelineX(n.end),y=laneY(n.lane)+h*.28f;
                if(right<b.minX||x>b.maxX)continue;r.drawRect({x,y,std::max(x+8,right),y+h*.45f},int(i)==selected?FVector4{.95f,.7f,.2f,1}:accent);}
            if(dragging&&draggedNote<0){const float x=timelineX(std::min(start,finish)),right=timelineX(std::max(start,finish)),y=laneY(lane)+h*.28f;r.drawRect({x,y,std::max(x+8,right),y+h*.45f},FVector4{.95f,.7f,.2f,1});}
            const float x=timelineX(wave->position);if(x>=b.minX&&x<=b.maxX)r.drawRect({x,b.minY+24,x+2,b.maxY},color("$color.text.primary"));
        }
        r.popClip();
    }
};

class ChartDesk {
public:
    AudioEditorSession& session;
    chart::Document document;chart::History history;chart::Judge judge;
    VBox* page=nullptr;Waveform* wave=nullptr;ChartCanvas* canvas=nullptr;
    TextLabel *status=nullptr,*audioName=nullptr,*positionLabel=nullptr,*feedbackLabel=nullptr;
    CheckBox* follow=nullptr;ComboBox* draftDensity=nullptr,*leadInChoice=nullptr;
    double leadIn=.5;std::array<TextLabel*,8> feedbackLines{};
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
            auto* r=row(page,34);button(r,L"选择音乐…","chart_music_open",[this]{if(picker){const auto p=picker();if(!p.empty())requestMusic(p);}},110);
            r->addWidget(label(L"BPM"),38);bpm=field(r,"chart_bpm",L"120",76);r->addWidget(label(L"节拍起点 / ms"),100);offset=field(r,"chart_offset",L"0",82);
            r->addWidget(label(L"吸附"),36);snap=new ComboBox();snap->setId("chart_snap");snap->setItems({L"1 拍",L"1/2 拍",L"1/4 拍",L"1/8 拍",L"1/16 拍"});snap->setSelectedIndex(2);r->addWidget(snap,95);
            auto commit=[this]{try{const double b=parse(bpm->getText()),o=parse(offset->getText())/1000;if(b<20 || b>1000 || std::abs(o)>600)throw std::runtime_error("settings");if(b!=document.bpm||o!=document.offset)changeTiming(b,o);}catch(...){bpm->setText(number(document.bpm));offset->setText(number(document.offset*1000));}wave->markDirty();canvas->markDirty();};
            bpm->setOnSubmit([commit](const std::wstring&){commit();});bpm->setOnFocusLostNotify(commit);offset->setOnSubmit([commit](const std::wstring&){commit();});offset->setOnFocusLostNotify(commit);
            auto* timing=row(page,30);
            button(timing,L"BPM ÷2","chart_bpm_half",[this]{changeTiming(document.bpm/2,document.offset);},76);
            button(timing,L"BPM ×2","chart_bpm_double",[this]{changeTiming(document.bpm*2,document.offset);},76);
            button(timing,L"−1ms","chart_offset_minus",[this]{changeTiming(document.bpm,document.offset-.001);},65);
            button(timing,L"+1ms","chart_offset_plus",[this]{changeTiming(document.bpm,document.offset+.001);},65);
            button(timing,L"此处设为第1拍","chart_beat_zero",[this]{changeTiming(document.bpm,wave->position);},125);
            button(timing,L"重新识别节奏","chart_analyze",[this]{if(!source.empty())requestMusic(source);},110);
            auto* draft=row(page,30);draft->addWidget(label(L"节奏草稿密度"),100);
            draftDensity=new ComboBox();draftDensity->setId("chart_draft_density");draftDensity->setItems({L"稀疏 · 整拍",L"标准 · 半拍",L"密集 · 1/4拍"});draftDensity->setSelectedIndex(1);draft->addWidget(draftDensity,135);
            button(draft,L"生成节奏草稿","chart_generate",[this]{generateDraft();},120);
            draft->addWidget(label(L"替换当前音符 · 可撤销 · 生成后精修"));
            auto* file=row(page,36);file->addWidget(label(L"谱面虚拟路径"),105);
            const auto stamp=std::chrono::system_clock::now().time_since_epoch().count();path=field(file,"chart_path",L"audio/charts/Chart_"+std::to_wstring(stamp)+L".chart.json",0);
            button(file,L"打开谱面","chart_load",[this]{loadAsset();},90);button(file,L"保存谱面","chart_save",[this]{saveAsset();},90);
            button(file,L"资产目录…","chart_folder",[this]{if(folder){const auto chosen=folder();if(!chosen.empty())assetOverride=chosen;}},90);
        }
        wave=new Waveform();wave->setId("chart_waveform");wave->editableRange=false;wave->showSelection=false;wave->emptyText=L"音乐波形 · 滚轮缩放，中键拖动轨道平移";wave->onSeek=[this](double t){seek(t);};wave->navigable=!preview;wave->contentInset=preview?76:0;
        wave->overlay=[this](IRenderBackend& r){auto area=wave->getWorldBounds();area.minX+=wave->contentInset;ChartCanvas::drawGrid(r,document,color("$color.text.muted"),color("$color.border"),area,wave->origin,std::max(.001,wave->visible),preview?4:1<<snap->getSelectedIndex());};
        page->addWidget(wave,preview?80:106);
        auto* transport=row(page,36);
        if(preview){leadInChoice=new ComboBox();leadInChoice->setId("chart_leadin");leadInChoice->setItems({L"立即开始",L"准备 0.5秒",L"准备 2秒"});leadInChoice->setSelectedIndex(1);transport->addWidget(leadInChoice,125);button(transport,L"重新开始","chart_restart",[this]{start();},110);button(transport,L"暂停 / 继续","chart_pause",[this]{togglePause();},120);}
        else{button(transport,L"运行谱面","chart_run",[this]{runSnapshot();},110);button(transport,L"试听音乐","chart_listen",[this]{listen();},100);button(transport,L"停止试听","chart_listen_stop",[this]{stop();},100);
            button(transport,L"撤销","chart_undo",[this]{if(history.move(document,true)){dirty=true;canvas->selected=-1;updateSettings();}},70);button(transport,L"重做","chart_redo",[this]{if(history.move(document,false)){dirty=true;canvas->selected=-1;updateSettings();}},70);}
        canvas=new ChartCanvas();canvas->setId(preview?"chart_player":"chart_editor");canvas->document=&document;canvas->history=&history;canvas->judge=&judge;canvas->wave=wave;canvas->player=preview;canvas->seek=[this](double t){seek(t);};
        canvas->changed=[this]{dirty=true;const int selected=canvas->selected;if(selected>=0 && selected<int(document.notes.size())){const auto& n=document.notes[selected];if(!voice)seek(n.time);laneInput->setSelectedIndex(n.lane);startInput->setText(number(n.time,3));endInput->setText(number(n.end,3));}};
        page->addWidget(canvas,preview?360:260);
        positionLabel=label(L"歌曲位置 0.000s · 第1小节第1拍","chart_position");page->addWidget(positionLabel,28);
        if(!preview){
            page->addWidget(label(L"共享秒/小节:拍标尺 · 点击波形/谱面标尺定位 · 中键平移 · 滚轮同步缩放"),24);
            auto* nav=row(page,28);follow=new CheckBox();follow->setId("chart_follow");follow->setText(L"跟随歌曲播放");follow->setChecked(true);nav->addWidget(follow,135);
            nav->addWidget(label(L"定位 / s"),60);auto* at=field(nav,"chart_seek",L"0",85);at->setOnSubmit([this](const std::wstring& value){try{seek(parse(value));}catch(...){}});
            button(nav,L"回到播放位置","chart_center",[this]{wave->origin=std::clamp(wave->position-wave->visible/4,0.0,std::max(0.0,wave->duration-wave->visible));wave->markDirty();canvas->markDirty();},120);
            auto* edit=row(page,36);laneInput=new ComboBox();laneInput->setItems({L"D",L"F",L"J",L"K"});laneInput->setSelectedIndex(0);edit->addWidget(laneInput,65);
            edit->addWidget(label(L"起点 / s"),68);startInput=field(edit,"chart_note_start",L"0",95);edit->addWidget(label(L"终点 / s"),68);endInput=field(edit,"chart_note_end",L"0",95);
            button(edit,L"添加 / 应用","chart_note_apply",[this]{applyNote();},110);
         }else{page->addWidget(label(L"横向四轨 D F J K · 在左侧判定线按键 · Esc 暂停 · 失焦暂停"),28);
            auto* responses=new VBox();responses->setSpacing(0);responses->setId("chart_feedback_list");
            for(size_t i=0;i<feedbackLines.size();++i){feedbackLines[i]=label(i==0?L"每个音符显示准确度与早/晚误差":L"",i==0?"chart_feedback":"chart_feedback_"+std::to_string(i));responses->addWidget(feedbackLines[i],16);}
            feedbackLabel=feedbackLines[0];page->addWidget(responses,128);}
        status=label(message,"chart_status");page->addWidget(status,38);
    }
    ~ChartDesk(){release();}
    static Widget* findControl(Widget* parent,const char* id){if(parent->getId()==id)return parent;for(auto* child:parent->getChildren())if(auto* found=findControl(child,id))return found;return nullptr;}
    TextInput* field(HBox* r,const char* id,const std::wstring& value,float width){auto* input=new TextInput();input->setId(id);input->setText(value);r->addWidget(input,width);return input;}
    struct LoadedMusic {
        std::shared_ptr<ayt::resource::Audio> music;
        std::shared_ptr<std::vector<float>> pcm;
        std::vector<float> peaks;
        rhythm::Analysis analysis;
        std::string file;
        double duration=0;
        uint32_t rate=48000,channels=2;
    };
    std::future<LoadedMusic> loading;
    std::shared_ptr<std::atomic<bool>> loadCancel;
    std::optional<chart::Document> pendingDocument;
    std::string pendingChartFile,pendingChartBytes,pendingRoot,previewSongName;
    bool lostFocusDuringLoad=false;
    uint64_t settingsRevision=0,loadRevision=0;
    rhythm::Analysis analysis;
    std::shared_ptr<std::vector<float>> preparedPcm;
    uint32_t preparedRate=48000,preparedChannels=2;
    std::future<std::string> preparingPreview;
    std::string cachedPreviewAudio;
    chart::Document previewSnapshot;
    static LoadedMusic decodeMusic(const std::string& file,uint32_t rate,bool detect,
                                   const std::shared_ptr<std::atomic<bool>>& cancel={}){
        rhythm::checkCancelled(cancel);ayt::resource::AudioLoader loader;
        LoadedMusic out;out.file=file;out.music=std::dynamic_pointer_cast<ayt::resource::Audio>(loader.load(file));
        const auto& m=out.music;
        if(!m||!m->getData()||(m->getChannels()!=1&&m->getChannels()!=2)||!m->getSampleRate()||
           (m->getBitsPerSample()!=16&&m->getBitsPerSample()!=32))throw std::runtime_error("audio decode failed");
        const size_t bytes=m->getBitsPerSample()/8,channels=m->getChannels(),frames=m->getDataSize()/bytes/channels;
        out.duration=double(frames)/m->getSampleRate();out.rate=rate;out.channels=uint32_t(channels);
        if(!frames||out.duration>600||m->getDataSize()>256ull*1024*1024)throw std::runtime_error("music limit: 600 seconds / 256 MiB PCM");
        auto read=[&](size_t f,unsigned c){const auto* ptr=m->getData()+(f*channels+c)*bytes;float sample=0;
            if(bytes==4)std::memcpy(&sample,ptr,4);else{int16_t v;std::memcpy(&v,ptr,2);sample=v/32768.f;}return std::isfinite(sample)?sample:0.f;};
        out.peaks.assign(4096,0);
        for(size_t f=0;f<frames;++f){if(f%65536==0)rhythm::checkCancelled(cancel);
            for(unsigned c=0;c<channels;++c){auto& peak=out.peaks[std::min<size_t>(4095,f*4096/frames)];peak=std::max(peak,std::abs(read(f,c)));}}
        if(detect)out.analysis=rhythm::analyze(frames,unsigned(channels),m->getSampleRate(),read,cancel);
        // Normalize on the worker: registerClip only copies PCM on the owner thread.
        const size_t outFrames=size_t(std::round(out.duration*rate));
        if(outFrames*channels*sizeof(float)>256ull*1024*1024)throw std::runtime_error("normalized music exceeds 256 MiB");
        out.pcm=std::make_shared<std::vector<float>>(outFrames*channels);
        for(size_t f=0;f<outFrames;++f){if(f%65536==0)rhythm::checkCancelled(cancel);
            const double source=double(f)*m->getSampleRate()/rate;const size_t a=std::min(frames-1,size_t(source)),b=std::min(frames-1,a+1);const float alpha=float(source-a);
            for(unsigned c=0;c<channels;++c)(*out.pcm)[f*channels+c]=read(a,c)+(read(b,c)-read(a,c))*alpha;}
        return out;
    }
    void stop(){if(valid()&&voice)bound->stop(voice);voice=0;running=paused=countdown=false;judge.held.fill(false);}
    void releaseMusic(){stop();if(valid()&&clip)bound->releaseClip(clip);clip=0;bound=nullptr;music.reset();preparedPcm.reset();cachedPreviewAudio.clear();}
    void release(){if(loadCancel)loadCancel->store(true);if(loading.valid()){try{loading.get();}catch(...){}}
        if(preparingPreview.valid()){try{preparingPreview.get();}catch(...){}}releaseMusic();}
    bool busy()const{return loading.valid()||preparingPreview.valid();}
    void updateSettings(){if(bpm){bpm->setText(number(document.bpm,2));offset->setText(number(document.offset*1000,2));}wave->markDirty();canvas->markDirty();}
    void changeTiming(double b,double o){if(b<20||b>1000||std::abs(o)>600)return;
        history.remember(document);document.bpm=b;document.offset=o;++settingsRevision;dirty=true;updateSettings();}
    void adopt(LoadedMusic next,const chart::Document* expected,bool detect){
        if(expected)expected->validate(next.duration);
        else{auto candidate=document;candidate.audio=next.file;candidate.validate(next.duration);}
        auto* e=engine();AudioClipDesc desc;desc.sampleRate=next.rate;desc.channels=uint16_t(next.channels);
        desc.frameCount=uint32_t(next.pcm->size()/next.channels);desc.pcmData=next.pcm->data();
        const auto nextClip=e?e->registerClip(desc):0;if(e&&!nextClip)throw std::runtime_error("audio registration failed");
        releaseMusic();music=std::move(next.music);preparedPcm=std::move(next.pcm);preparedRate=next.rate;preparedChannels=next.channels;
        clip=nextClip;bound=e;generation=e?e->lifecycleGeneration():0;source=next.file;analysis=std::move(next.analysis);
        if(expected){document=*expected;history={};dirty=false;}else{
            document.audio=chart::utf8(std::filesystem::absolute(std::filesystem::path(source)));document.audioRelative=false;dirty=true;
            // Edits made while analysis runs have priority over automatic estimates.
            if(detect&&analysis.reliable()&&settingsRevision==loadRevision){document.bpm=analysis.bpm;document.offset=analysis.offset;}
        }
        wave->duration=next.duration;wave->origin=0;wave->visible=std::min(12.0,wave->duration);wave->position=0;wave->peaks=std::move(next.peaks);
        pinnedRoot=root();canvas->selected=-1;judge.reset(document);updateSettings();
        audioName->setText(std::filesystem::path(source).filename().wstring()+L" · "+number(wave->duration)+L" s");
        message=detect?(analysis.reliable()&&settingsRevision!=loadRevision?L"已完成节奏分析 · 保留你的手动 BPM / 起点":analysis.reliable()?L"已估算 BPM "+number(analysis.bpm,2)+L" · 可信度 "+number(analysis.confidence*100,0)+L"% · 可微调节拍起点":L"节奏不够稳定，保留当前 BPM · 请手动校准"):L"音乐已载入";
    }
    bool loadMusic(const std::string& file,const chart::Document* expected=nullptr){
        if(busy())return false;try{auto* e=engine();adopt(decodeMusic(file,e?e->outputSampleRate():48000,false),expected,false);return true;}
        catch(const std::exception& ex){message=L"音乐载入失败："+wide(ex.what());return false;}}
    void requestMusic(const std::string& file,std::optional<chart::Document> expected={},
                      std::string chartFile={},std::string chartBytes={}){
        if(busy()){message=L"正在准备，请稍候";return;}
        lostFocusDuringLoad=false;loadCancel=std::make_shared<std::atomic<bool>>(false);pendingDocument=std::move(expected);
        pendingChartFile=std::move(chartFile);pendingChartBytes=std::move(chartBytes);pendingRoot=root();loadRevision=settingsRevision;
        auto* e=engine();const uint32_t rate=e?e->outputSampleRate():48000;const bool detect=!preview;
        const auto cancel=loadCancel;loading=std::async(std::launch::async,[file,rate,detect,cancel]{return decodeMusic(file,rate,detect,cancel);});
        message=L"正在后台解码音乐、提取波形与节奏…";
    }
    void seek(double t){wave->position=std::clamp(t,0.0,wave->duration);if(valid()&&voice)bound->setVoicePositionFrames(voice,uint64_t(wave->position*bound->outputSampleRate()));wave->markDirty();canvas->markDirty();}
    void generateDraft(){if(busy()||!music){message=L"先导入歌曲并等待分析完成";return;}
        try{auto next=document;next.notes=rhythm::draft(analysis,document,wave->duration,draftDensity->getSelectedIndex());
            if(next.notes.empty())throw std::runtime_error("no usable rhythmic onsets; edit manually");next.validate(wave->duration);
            history.remember(document);document=std::move(next);canvas->selected=-1;dirty=true;message=L"已生成点按节奏草稿 · 可撤销、移动及改成长按";canvas->markDirty();}
        catch(const std::exception& e){message=L"生成失败："+wide(e.what());}}
    static std::string cacheAudio(const std::shared_ptr<std::vector<float>>& pcm,uint32_t rate,uint32_t channels){
        const auto folder=std::filesystem::temp_directory_path()/"AYAudioChartPreview";std::filesystem::create_directories(folder);
        static std::atomic<uint64_t> serial{0};const auto file=folder/("Music_"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count())+"_"+std::to_string(++serial)+".wav");
        std::ofstream out(file,std::ios::binary|std::ios::trunc);const uint32_t size=uint32_t(pcm->size()*sizeof(int16_t));
        auto u16=[&](uint16_t v){out.put(char(v));out.put(char(v>>8));};auto u32=[&](uint32_t v){u16(uint16_t(v));u16(uint16_t(v>>16));};
        out.write("RIFF",4);u32(36+size);out.write("WAVEfmt ",8);u32(16);u16(1);u16(uint16_t(channels));u32(rate);u32(rate*channels*2);u16(uint16_t(channels*2));u16(16);out.write("data",4);u32(size);
        // The current resource decoder accepts integer PCM WAV and normalizes to S16.
        // Buffer conversion avoids per-sample stream writes on long songs.
        std::vector<int16_t> block(8192);
        for(size_t offset=0;offset<pcm->size();offset+=block.size()){
            const size_t count=std::min(block.size(),pcm->size()-offset);
            for(size_t i=0;i<count;++i)block[i]=int16_t(std::clamp(std::lround((*pcm)[offset+i]*32768.f),-32768l,32767l));
            out.write(reinterpret_cast<const char*>(block.data()),std::streamsize(count*sizeof(int16_t)));
        }
        out.close();if(!out)throw std::runtime_error("cannot prepare preview audio cache");return chart::utf8(file);
    }
    void launchSnapshot(){
        auto snapshot=previewSnapshot;snapshot.audio=cachedPreviewAudio;snapshot.audioRelative=false;
        const auto temp=std::filesystem::temp_directory_path()/"AYAudioChartPreview";std::filesystem::create_directories(temp);
        static std::atomic<uint64_t> serial{0};const auto file=temp/("Preview_"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count())+"_"+std::to_string(++serial)+".chart.json");
        std::ofstream out(file,std::ios::binary|std::ios::trunc);auto json=snapshot.json();json["previewSongName"]=std::filesystem::path(source).filename().string();out<<json.dump(2);out.close();if(!out)throw std::runtime_error("cannot create preview snapshot");std::string error;
        if(!launch||!launch(chart::utf8(file),error))throw std::runtime_error(error.empty()?"preview executable unavailable":error);
        message=L"横向独立试玩已启动 · D F J K · 编辑内容保留";
    }
    void runSnapshot(){try{if(busy())return;checkReady();stop();previewSnapshot=document;
        if(!cachedPreviewAudio.empty()){launchSnapshot();return;}
        const auto pcm=preparedPcm;const auto rate=preparedRate,channels=preparedChannels;
        preparingPreview=std::async(std::launch::async,[pcm,rate,channels]{return cacheAudio(pcm,rate,channels);});
        message=L"正在后台准备可复用的试玩音频…";
    }catch(const std::exception& ex){message=L"运行失败："+wide(ex.what());}}
    void pollJobs(){
        if(loading.valid()&&loading.wait_for(std::chrono::seconds(0))==std::future_status::ready){
            try{auto result=loading.get();if(root()!=pendingRoot)throw std::runtime_error("project changed while loading music");
                adopt(std::move(result),pendingDocument?&*pendingDocument:nullptr,!preview);
                if(pendingDocument){lastFile=pendingChartFile;lastBytes=pendingChartBytes;message=L"谱面已载入";}
                if(preview){if(!previewSongName.empty())audioName->setText(wide(previewSongName)+L" · "+number(wave->duration)+L" s");start();if(lostFocusDuringLoad)togglePause();}
            }catch(const std::exception& e){message=L"音乐载入失败："+wide(e.what());}pendingDocument.reset();}
        if(preparingPreview.valid()&&preparingPreview.wait_for(std::chrono::seconds(0))==std::future_status::ready){
            try{cachedPreviewAudio=preparingPreview.get();launchSnapshot();}catch(const std::exception& e){message=L"运行失败："+wide(e.what());}}
    }

    void applyNote(){try{chart::Note note{laneInput->getSelectedIndex(),parse(startInput->getText()),parse(endInput->getText())};auto next=document;const int index=canvas->selected;if(index>=0 && index<int(next.notes.size()))next.notes[index]=note;else next.notes.push_back(note);next.validate(wave->duration);history.remember(document);document=std::move(next);dirty=true;canvas->selected=-1;message=L"音符已更新";canvas->markDirty();}catch(...){message=L"音符无效：起点 ≥ 0，终点 ≥ 起点，且同轨音符不能重叠";}}
    void checkReady(){if(busy())throw std::runtime_error("music is being prepared");document.validate(wave->duration);if(!music || !valid() || !clip)throw std::runtime_error("select music and initialize audio first");}
    chart::Document stored()const{auto result=document;if(!root().empty()){const auto base=std::filesystem::weakly_canonical(std::filesystem::u8path(root())),file=std::filesystem::weakly_canonical(std::filesystem::path(source));const auto rel=file.lexically_relative(base);if(!rel.empty() && *rel.begin()!=".."){result.audio=narrow(rel.generic_wstring());result.audioRelative=true;}}return result;}
    void saveAsset(){try{if(busy())throw std::runtime_error("music is being prepared");
        document.validate(wave->duration);if(!pinnedRoot.empty() && root()!=pinnedRoot)throw std::runtime_error("project changed; return to the original project");
        const auto logical=narrow(path->getText());if(!logical.ends_with(".chart.json"))throw std::runtime_error("use .chart.json extension");
        const auto file=chart::containedPath(root(),logical);if(std::filesystem::exists(file) && (chart::utf8(file)!=lastFile || chart::read(file)!=lastBytes))throw std::runtime_error("file exists or changed externally; use another path");
        const auto bytes=stored().json().dump(2);std::filesystem::create_directories(file.parent_path());if(!ayt::io::File::atomicWrite(file.string(),bytes.data(),bytes.size()))throw std::runtime_error("cannot write chart");
        lastFile=chart::utf8(file);lastBytes=bytes;pinnedRoot=root();dirty=false;message=L"谱面已保存："+path->getText();if(saved)saved(logical);
    }catch(const std::exception& ex){message=L"保存失败："+wide(ex.what());}}
    bool loadFile(const std::filesystem::path& file,bool asynchronous=false){try{if(busy())return false;const auto bytes=chart::read(file);const auto json=nlohmann::json::parse(bytes);auto next=chart::Document::fromJson(json);
        previewSongName=json.value("previewSongName",std::string{});
        const auto audioFile=next.audioRelative?chart::containedPath(root(),next.audio):std::filesystem::u8path(next.audio);
        if(asynchronous){requestMusic(audioFile.string(),next,chart::utf8(file),bytes);return true;}
        if(!loadMusic(audioFile.string(),&next))return false;document=std::move(next);history={};judge.reset(document);canvas->selected=-1;lastFile=chart::utf8(file);lastBytes=bytes;dirty=false;
        if(bpm){bpm->setText(number(document.bpm));offset->setText(number(document.offset*1000));}message=L"谱面已载入";canvas->markDirty();return true;
    }catch(const std::exception& ex){message=L"谱面载入失败："+wide(ex.what());return false;}}
    void loadAsset(){try{const auto logical=narrow(path->getText());if(!logical.ends_with(".chart.json"))throw std::runtime_error("use .chart.json extension");loadFile(chart::containedPath(root(),logical),true);}catch(const std::exception& ex){message=L"谱面载入失败："+wide(ex.what());}}
    void listen(){try{checkReady();stop();voice=bound->play(clip,AudioBus::Music);if(voice)bound->setVoicePositionFrames(voice,uint64_t(wave->position*bound->outputSampleRate()));message=L"正在试听音乐";}catch(const std::exception& ex){message=L"试听失败："+wide(ex.what());}}
    void start(){try{checkReady();stop();judge.reset(document);epoch=std::chrono::steady_clock::now();leadIn=leadInChoice?(leadInChoice->getSelectedIndex()==0?0:leadInChoice->getSelectedIndex()==1?.5:2):.5;clock=canvas->now=-leadIn;running=countdown=true;message=L"准备 · D F J K";}catch(const std::exception& ex){message=L"运行失败："+wide(ex.what());}}
    double time(){
        if(paused)return pausedTime;if(countdown)return -leadIn+elapsed(epoch);
        if(!running || !valid() || !voice)return clock;
        VoiceRuntime state;if(bound->voiceState(voice,state)){
            if(state.positionFrames!=clockFrame){clockFrame=state.positionFrames;anchor=double(clockFrame)/bound->outputSampleRate();frameEpoch=std::chrono::steady_clock::now();}
            clock=std::max(clock,std::min(wave->duration,anchor+std::min(.05,elapsed(frameEpoch))));
        }else clock=wave->duration;
        return clock;
    }
    void togglePause(){if(!running)return;if(!paused){pausedTime=time();paused=true;judge.held.fill(false);for(size_t i=0;i<judge.states.size();++i)if(judge.states[i]==chart::Judge::Holding){judge.finish(i,chart::Judge::Miss);judge.notify(i,chart::Judge::Miss,0,pausedTime,true);}if(valid() && voice)bound->setVoicePaused(voice,true);message=L"已暂停 · 点击继续或按 Esc";}
        else{paused=false;if(countdown)epoch=std::chrono::steady_clock::now()-std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(pausedTime+leadIn));else{anchor=pausedTime;frameEpoch=std::chrono::steady_clock::now();if(valid() && voice)bound->setVoicePaused(voice,false);}message=L"试玩继续";}}
    bool key(int key,bool down){if(!preview)return false;if(key==27){if(down)togglePause();return true;}const std::string keys="DFJK";const auto index=keys.find(char(key));if(index==std::string::npos)return false;if(running && !paused && !countdown){judge.key(document,int(index),down,time());canvas->markDirty();}return true;}
    void focusLost(){if(preview&&loading.valid())lostFocusDuringLoad=true;if(preview && running && !paused)togglePause();canvas->onCaptureCancelled();}
    void tick(){
        pollJobs();
        if(!preview){canvas->division=1<<snap->getSelectedIndex();
            for(const char* id:{"chart_music_open","chart_load","chart_run","chart_generate","chart_analyze","chart_save"})
                if(auto* control=dynamic_cast<Button*>(findControl(page,id)))control->setEnabled(!busy());
            if(valid()&&voice){VoiceRuntime state;if(bound->voiceState(voice,state)){wave->position=double(state.positionFrames)/bound->outputSampleRate();wave->markDirty();}else voice=0;}
            if(voice&&follow&&follow->isChecked()&&(wave->position<wave->origin||wave->position>wave->origin+wave->visible*.8)){
                wave->origin=std::clamp(wave->position-wave->visible*.2,0.0,std::max(0.0,wave->duration-wave->visible));wave->markDirty();}
            positionLabel->setText(L"歌曲位置 "+number(wave->position,3)+L"s · 小节:拍 "+ChartCanvas::beatLabel(document,wave->position)+L" · 第"+number((wave->position-document.offset)*document.bpm/60+1,3)+L"拍");
            status->setText((dirty?L"未保存 · ":L"")+message+L" · 音符 "+std::to_wstring(document.notes.size()));canvas->markDirty();return;}
        if(running && !valid()){stop();message=L"音频设备已关闭，请重新载入音乐";}
        if(running && !paused){double t=time();if(countdown && t>=0){countdown=false;voice=bound->play(clip,AudioBus::Music);if(!voice){stop();message=L"音乐启动失败";}else{clock=anchor=0;clockFrame=0;frameEpoch=std::chrono::steady_clock::now();t=0;}}
            if(running){canvas->now=t;judge.update(document,t);canvas->markDirty();if(!countdown && t>=wave->duration){judge.update(document,wave->duration+.150001);stop();message=L"试玩完成";}}
        }
        wave->position=std::max(0.0,canvas->now);wave->visible=6;wave->origin=canvas->now;wave->markDirty();
        positionLabel->setText(L"歌曲位置 "+number(wave->position,3)+L"s · 小节:拍 "+ChartCanvas::beatLabel(document,wave->position)+L" · 总准确度 "+number(judge.totalAccuracy(),1)+L"%");
        size_t response=0;for(auto it=judge.feedback.rbegin();it!=judge.feedback.rend()&&response<feedbackLines.size();++it,++response){const auto& note=document.notes[it->note];
            const auto grade=it->result==chart::Judge::Perfect?L"Perfect":it->result==chart::Judge::Good?L"Good":L"Miss";
            feedbackLines[response]->setText(L"#"+std::to_wstring(it->note+1)+L" "+std::wstring(1,L"DFJK"[note.lane])+(it->release?L" 尾端 ":L" 起点 ")+grade+L" · "+number(it->accuracy,1)+L"% · "+(it->error<0?L"早 ":L"晚 ")+number(std::abs(it->error)*1000,1)+L"ms");}
        for(size_t i=response;i<feedbackLines.size();++i)feedbackLines[i]->setText(i==0?L"每个音符显示准确度与早/晚误差":L"");
        status->setText(message+L" · Perfect "+std::to_wstring(judge.count(chart::Judge::Perfect))+L" / Good "+std::to_wstring(judge.count(chart::Judge::Good))+L" / Miss "+std::to_wstring(judge.count(chart::Judge::Miss))+L" · Combo "+std::to_wstring(judge.combo)+L" · 误差 "+number(judge.lastError*1000,1)+L" ms");
    }
};
