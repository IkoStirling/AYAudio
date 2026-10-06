#include "PerformanceCanvas.inl"
class PerformanceDesk {
public:
    using Tick=performance::Tick;
    performance::EditState state;performance::History history;performance::Step step;performance::Take take;
    AudioEditorSession& session;std::function<AudioEngine*()> engine;
    std::function<std::string()> assets,folder;std::function<performance::Instrument()> instrument;
    std::function<void(const std::string&)> saved;std::function<void()> releaseInput;
    PerformanceCanvas *timeline=nullptr,*roll=nullptr;
    TextLabel* status=nullptr;TextInput *bpm=nullptr,*cursor=nullptr,*rangeEnd=nullptr,*path=nullptr,*trackName=nullptr,*trackGain=nullptr,*clipName=nullptr;
    TextInput *clipPosition=nullptr,*clipOffset=nullptr,*clipLength=nullptr,*clipRepeats=nullptr,*notePitch=nullptr,*noteStart=nullptr,*noteLength=nullptr,*noteVelocity=nullptr,*master=nullptr;
    ComboBox *mode=nullptr,*grid=nullptr,*length=nullptr,*preroll=nullptr;
    CheckBox *snapEnds=nullptr,*metronome=nullptr,*mute=nullptr,*solo=nullptr;
    std::vector<Widget*> editControls;
    bool recording=false,dirty=false,refreshing=false;
    double recordEpoch=0,recordStart=0,recordLimit=0,preBeats=0;
    performance::EditState recordBefore;
    std::future<performance::RenderResult> job;std::shared_ptr<std::atomic<bool>> cancel;
    enum Job {None,Playback,Recording,Export};Job jobKind=None;
    AudioEngine* bound=nullptr;uint64_t generation=0;AudioClipId playbackClip=0;VoiceHandle voice=0;
    double playbackBegin=0,restoreCursor=0;bool clipPreview=false;
    std::string rootOverride,documentRoot,baseline,savedFile,exportFile,exportLogical,jobRoot;
    std::wstring message=L"实时录制吸附起点、保留按键时长；步进录入使用固定时值";
    bool busy()const{return recording||jobKind!=None||voice!=0;}
    bool valid()const{auto* e=engine();return e&&e==bound&&e->lifecycleGeneration()==generation;}
    double now()const{return session.performanceTime();}
    std::string root()const{return rootOverride.empty()?assets():rootOverride;}
    static std::wstring wide(const std::string& v){try{return std::filesystem::u8path(v).wstring();}catch(...){return L"文件或音频操作失败";}}
    static std::string utf8(const std::wstring& v){return chart::utf8(std::filesystem::path(v));}
    TextInput* field(HBox* row,const char* id,const std::wstring& value,float width){auto* f=new TextInput();f->setId(id);f->setText(value);row->addWidget(f,width);return f;}
    ComboBox* combo(HBox* r,const char* id,const std::vector<std::wstring>& items,int selected,float width){auto* c=new ComboBox();c->setId(id);c->setItems(items);c->setSelectedIndex(selected);r->addWidget(c,width);return c;}
    CheckBox* check(HBox* r,const char* id,const wchar_t* text,bool value,float width){auto* c=new CheckBox();c->setId(id);c->setText(text);c->setChecked(value);r->addWidget(c,width);return c;}
    void remember(){step.finish(state,history);history.remember(state);dirty=true;if(documentRoot.empty())documentRoot=root();}
    void finishGesture(){timeline->onCaptureCancelled();roll->onCaptureCancelled();}
    void modify(const std::function<void()>& action){if(busy())return;remember();const auto old=state;try{action();state.document.validate();}catch(const std::exception& e){state=old;history.undo.pop_back();message=wide(e.what());}refresh();}
    PerformanceDesk(AudioEditorSession& owner,VBox* page,std::function<AudioEngine*()> getEngine,std::function<std::string()> getAssets,std::function<std::string()> getFolder,
                    std::function<performance::Instrument()> getInstrument,std::function<void(const std::string&)> onSaved,std::function<void()> release)
        :session(owner),engine(std::move(getEngine)),assets(std::move(getAssets)),folder(std::move(getFolder)),instrument(std::move(getInstrument)),saved(std::move(onSaved)),releaseInput(std::move(release)) {
        state.track=state.document.addTrack().id;
        auto* transport=row(page,32);
        button(transport,L"● 录制片段","performance_record",[this]{startRecord();},104);
        button(transport,L"■ 停止","performance_stop",[this]{stop();},76);
        button(transport,L"▶ 编排","performance_play",[this]{play(false,false);},76);
        button(transport,L"试听片段","performance_audition",[this]{play(true,false);},82);
        button(transport,L"试听区间","performance_range_play",[this]{play(false,true);},82);
        button(transport,L"撤销","performance_undo",[this]{undo(true);},60);button(transport,L"重做","performance_redo",[this]{undo(false);},60);
        mode=combo(transport,"performance_mode",{L"实时录制",L"步进录入"},0,110);
        mode->setOnSelectionChanged([this](int){if(releaseInput)releaseInput();step.finish(state,history);refresh();});
        auto* settings=row(page,30);settings->addWidget(label(L"BPM"),34);bpm=field(settings,"performance_bpm",L"120",52);
        settings->addWidget(label(L"起点吸附"),62);grid=combo(settings,"performance_grid",{L"1 拍",L"半拍",L"1/4 拍",L"1/8 拍"},2,86);
        settings->addWidget(label(L"步进时值"),62);length=combo(settings,"performance_length",{L"1 拍",L"半拍",L"1/4 拍",L"1/8 拍"},0,86);
        snapEnds=check(settings,"performance_snap_ends",L"结束吸附",false,92);metronome=check(settings,"performance_metronome",L"节拍器",true,82);
        preroll=combo(settings,"performance_preroll",{L"无预备拍",L"预备 1 小节",L"预备 2 小节"},2,118);
        auto* timing=row(page,30);timing->addWidget(label(L"光标 / 拍"),70);cursor=field(timing,"performance_cursor",L"0",64);
        timing->addWidget(label(L"录制 / 区间终点"),112);rangeEnd=field(timing,"performance_range_end",L"16",64);
        timing->addWidget(label(L"编排音量 %"),84);master=field(timing,"performance_master",L"80",55);
        button(timing,L"量化片段","performance_quantize",[this]{modify([this]{state.document.quantize(state.clip,1<<grid->getSelectedIndex(),snapEnds->isChecked());});},86);
        button(timing,L"全选音符","performance_select_all",[this]{roll->selected.clear();if(auto* c=state.document.clip(state.clip))for(const auto& n:c->notes)roll->selected.push_back(n.id);refresh();},86);
        button(timing,L"音域 −","performance_pitch_down",[this]{roll->lowPitch=std::max(0,roll->lowPitch-12);roll->highPitch=roll->lowPitch+24;roll->markDirty();},66);
        button(timing,L"音域 +","performance_pitch_up",[this]{roll->lowPitch=std::min(103,roll->lowPitch+12);roll->highPitch=roll->lowPitch+24;roll->markDirty();},66);
        auto commitSettings=[this]{if(busy()||refreshing)return;try{const double nextBpm=parse(bpm->getText());const Tick at=performance::ticks(parse(cursor->getText()));const float gain=float(parse(master->getText())/100);if(nextBpm<20||nextBpm>1000||gain<0||gain>2)throw std::runtime_error("settings");if(nextBpm!=state.document.bpm||gain!=state.document.master)modify([&]{state.document.bpm=nextBpm;state.document.master=gain;});state.cursor=at;}catch(...){message=L"检查 BPM、光标和音量数值";}refresh();};
        for(auto* f:{bpm,cursor,master}){f->setOnSubmit([commitSettings](const std::wstring&){commitSettings();});f->setOnFocusLostNotify(commitSettings);}
        grid->setOnSelectionChanged([this](int index){timeline->division=roll->division=1<<index;refresh();});
        auto* navigation=row(page,26);navigation->addWidget(label(L"拖动标尺 / 时间轴空白：左右平移 · 滚轮缩放 · 左侧滚轮切换音域 / 轨道"));
        button(navigation,L"‹","performance_pan_left",[this]{timeline->origin=std::max(0.0,timeline->origin-timeline->visible/2);timeline->markDirty();},30);
        button(navigation,L"›","performance_pan_right",[this]{timeline->origin+=timeline->visible/2;timeline->markDirty();},30);
        button(navigation,L"回到光标","performance_pan_cursor",[this]{timeline->origin=std::max(0.0,performance::beats(state.cursor)-timeline->visible/4);roll->origin=0;timeline->markDirty();roll->markDirty();},80);
        timeline=new PerformanceCanvas();timeline->setId("performance_timeline");timeline->state=&state;timeline->history=&history;timeline->changed=[this]{dirty=true;refresh();};page->addWidget(timeline,144);
        auto* tracks=row(page,30);tracks->addWidget(label(L"点击左侧轨道选中"),132);
        button(tracks,L"＋轨道","performance_add_track",[this]{modify([this]{state.track=state.document.addTrack().id;state.clip=0;timeline->firstTrack=std::max(0,int(state.document.tracks.size())-2);});},70);
        trackName=field(tracks,"performance_track_name",L"轨道 1",105);trackGain=field(tracks,"performance_track_gain",L"100",48);tracks->addWidget(label(L"%"),18);
        mute=check(tracks,"performance_track_mute",L"静音",false,66);solo=check(tracks,"performance_track_solo",L"独奏",false,66);
        button(tracks,L"应用轨道","performance_track_apply",[this]{modify([this]{auto* t=state.document.track(state.track);if(t){t->name=utf8(trackName->getText());t->gain=float(parse(trackGain->getText())/100);}});},82);
        mute->setOnToggled([this](bool v){if(!refreshing)modify([this,v]{if(auto* t=state.document.track(state.track))t->mute=v;});});
        solo->setOnToggled([this](bool v){if(!refreshing)modify([this,v]{if(auto* t=state.document.track(state.track))t->solo=v;});});
        auto* clips=row(page,30);clipName=field(clips,"performance_clip_name",L"片段",96);
        clips->addWidget(label(L"位置"),32);clipPosition=field(clips,"performance_clip_position",L"0",48);
        clips->addWidget(label(L"裁剪起点"),62);clipOffset=field(clips,"performance_clip_offset",L"0",48);
        clips->addWidget(label(L"长度"),32);clipLength=field(clips,"performance_clip_length",L"4",48);
        clips->addWidget(label(L"循环"),32);clipRepeats=field(clips,"performance_clip_repeats",L"1",38);
        button(clips,L"应用","performance_clip_apply",[this]{modify([this]{if(auto* c=state.document.clip(state.clip)){c->name=utf8(clipName->getText());c->position=performance::ticks(parse(clipPosition->getText()));c->offset=performance::ticks(parse(clipOffset->getText()));c->length=performance::ticks(parse(clipLength->getText()));const double value=parse(clipRepeats->getText());if(value!=std::floor(value)||value<1||value>256)throw std::runtime_error("invalid repeat count");c->repeats=int(value);}});},58);
        button(clips,L"复制","performance_clip_copy",[this]{modify([this]{if(const auto* c=state.document.clip(state.clip)){const Tick position=c->position+c->length*c->repeats;state.clip=state.document.duplicate(c->id,state.track,position).id;}});},58);
        button(clips,L"删除","performance_clip_delete",[this]{modify([this]{for(auto& t:state.document.tracks)t.clips.erase(std::remove_if(t.clips.begin(),t.clips.end(),[this](const auto& c){return c.id==state.clip;}),t.clips.end());state.clip=0;});},58);
        roll=new PerformanceCanvas();roll->setId("performance_piano_roll");roll->roll=true;roll->state=&state;roll->history=&history;roll->changed=[this]{dirty=true;refresh();};roll->additive=[this]{return session.uiModifiers()&2u;};page->addWidget(roll,176);
        auto* notes=row(page,30);notes->addWidget(label(L"音高"),32);notePitch=field(notes,"performance_note_pitch",L"60",40);notes->addWidget(label(L"起点 / 拍"),62);noteStart=field(notes,"performance_note_start",L"0",55);notes->addWidget(label(L"时值 / 拍"),62);noteLength=field(notes,"performance_note_length",L"1",55);notes->addWidget(label(L"力度 %"),54);noteVelocity=field(notes,"performance_note_velocity",L"80",45);
        button(notes,L"添加 / 应用","performance_note_apply",[this]{applyNote();},95);button(notes,L"复制音符","performance_note_copy",[this]{copyNotes();},78);button(notes,L"删除音符","performance_note_delete",[this]{deleteNotes();},78);
        auto* files=row(page,30);const auto stamp=std::chrono::system_clock::now().time_since_epoch().count();path=field(files,"performance_path",L"audio/performances/Take_"+std::to_wstring(stamp)+L".performance.json",0);
        button(files,L"打开","performance_load",[this]{loadAsset();},58);button(files,L"保存","performance_save",[this]{saveAsset();},58);button(files,L"导出 WAV","performance_export",[this]{exportWav();},82);button(files,L"导出区间","performance_export_range",[this]{exportWav(true);},82);button(files,L"目录…","performance_folder",[this]{if(!busy()&&folder){const auto chosen=folder();if(!chosen.empty())rootOverride=chosen;}refresh();},66);
        status=label(message,"performance_status");page->addWidget(status,26);
        editControls={transport,mode,bpm,cursor,rangeEnd,master,grid,length,snapEnds,metronome,preroll,tracks,clips,notes,files};refresh();
    }
    ~PerformanceDesk(){release();}
    void refresh(){if(!timeline||!roll||!status)return;refreshing=true;
        timeline->firstTrack=std::clamp(timeline->firstTrack,0,std::max(0,int(state.document.tracks.size())-1));
        bpm->setText(number(state.document.bpm,1));cursor->setText(number(performance::beats(state.cursor),3));master->setText(number(state.document.master*100,0));
        if(auto* t=state.document.track(state.track)){trackName->setText(wide(t->name));trackGain->setText(number(t->gain*100,0));mute->setChecked(t->mute);solo->setChecked(t->solo);}
        if(auto* c=state.document.clip(state.clip)){clipName->setText(wide(c->name));clipPosition->setText(number(performance::beats(c->position),3));clipOffset->setText(number(performance::beats(c->offset),3));clipLength->setText(number(performance::beats(c->length),3));clipRepeats->setText(std::to_wstring(c->repeats));
            if(roll->selected.size()==1)for(const auto& n:c->notes)if(n.id==roll->selected[0]){notePitch->setText(std::to_wstring(n.pitch));noteStart->setText(number(performance::beats(n.start),4));noteLength->setText(number(performance::beats(n.duration),4));noteVelocity->setText(number(n.velocity*100,0));}
        }length->setEnabled(mode->getSelectedIndex()==1&&!busy());timeline->locked=roll->locked=busy()||step.pending;
        timeline->division=roll->division=1<<grid->getSelectedIndex();timeline->markDirty();roll->markDirty();status->setText(message+(dirty?L" · 未保存":L""));
        const bool locked=busy();
        std::function<void(Widget*)> lock=[&](Widget* w){
            if(auto* input=dynamic_cast<TextInput*>(w))input->setReadOnly(locked);
            if(auto* button=dynamic_cast<Button*>(w))button->setEnabled(w->getId()=="performance_stop"||!locked);
            if(auto* check=dynamic_cast<CheckBox*>(w))check->setEnabled(!locked);
            if(auto* combo=dynamic_cast<ComboBox*>(w))combo->setEnabled(!locked&&(w!=length||mode->getSelectedIndex()==1));
            for(auto* child:w->getChildren())lock(child);
        };
        for(auto* control:editControls)lock(control);
        refreshing=false;
    }
    void applyNote(){modify([this]{auto* c=state.document.clip(state.clip);if(!c){performance::Clip next;next.id=state.document.allocate();next.position=state.cursor;next.instrument=instrument();state.document.track(state.track)->clips.push_back(next);state.clip=next.id;c=state.document.clip(state.clip);}
        const double p=parse(notePitch->getText());if(p!=std::floor(p)||p<0||p>127)throw std::runtime_error("invalid pitch");const Tick start=performance::ticks(parse(noteStart->getText())),duration=performance::ticks(parse(noteLength->getText()));const float velocity=float(parse(noteVelocity->getText())/100);
        if(roll->selected.empty()){performance::Note n{state.document.allocate(),int(p),start,duration,velocity,performance::beats(start),performance::beats(duration)};c->notes.push_back(n);roll->selected={n.id};}
        else for(auto& n:c->notes)if(std::find(roll->selected.begin(),roll->selected.end(),n.id)!=roll->selected.end()){n.velocity=velocity;if(roll->selected.size()==1){n.pitch=int(p);n.start=start;n.duration=duration;n.rawStart=performance::beats(start);n.rawDuration=performance::beats(duration);}}
        c->sourceLength=std::max(c->sourceLength,start+duration);c->length=std::max(c->length,c->sourceLength-c->offset);
    });}
    void copyNotes(){modify([this]{auto* c=state.document.clip(state.clip);if(!c)return;auto notes=c->notes;std::vector<uint64_t> selected;for(auto n:notes)if(std::find(roll->selected.begin(),roll->selected.end(),n.id)!=roll->selected.end()){n.id=state.document.allocate();n.start+=performance::PPQ/(1<<grid->getSelectedIndex());n.rawStart=performance::beats(n.start);c->sourceLength=std::max(c->sourceLength,n.start+n.duration);c->notes.push_back(n);selected.push_back(n.id);}c->length=std::max(c->length,c->sourceLength-c->offset);roll->selected=selected;});}
    void deleteNotes(){modify([this]{if(auto* c=state.document.clip(state.clip))c->notes.erase(std::remove_if(c->notes.begin(),c->notes.end(),[this](const auto& n){return std::find(roll->selected.begin(),roll->selected.end(),n.id)!=roll->selected.end();}),c->notes.end());roll->selected.clear();});}
    void undo(bool backwards){if(busy())return;if(releaseInput)releaseInput();finishGesture();step.finish(state,history);if(history.move(state,backwards)){dirty=true;roll->selected.clear();message=backwards?L"已撤销":L"已重做";refresh();}}
    double elapsed()const{return std::clamp(now()-recordEpoch-preBeats*60/state.document.bpm,0.0,recordLimit*60/state.document.bpm);}
    void note(int pitch,bool down,float velocity,double eventTime){if(recording){const double since=eventTime-recordEpoch-preBeats*60/state.document.bpm;if(since<0||since>recordLimit*60/state.document.bpm)return;take.note(state.document,pitch,down,std::clamp(since,0.0,recordLimit*60/state.document.bpm),velocity);}
        else if(mode->getSelectedIndex()==1&&!busy()){if(documentRoot.empty())documentRoot=root();if(step.note(state,instrument(),pitch,down,performance::PPQ/(1<<length->getSelectedIndex()),velocity,history)){dirty=true;refresh();}}
    }
    void sustain(bool down){if(recording&&now()-recordEpoch>=preBeats*60/state.document.bpm)take.sustain(down,elapsed());}
    void releaseNotes(){if(recording){for(int p=0;p<128;++p)if(take.held[p]>=0)take.note(state.document,p,false,elapsed(),1);take.sustain(false,elapsed());}step.finish(state,history);}
    void launch(const performance::Document& document,performance::RenderOptions options,Job kind){if(jobKind!=None)return;cancel=std::make_shared<std::atomic<bool>>(false);options.cancel=cancel;options.assets=root();options.builtinAssets=session.builtinAudioAssets();jobRoot=root();jobKind=kind;message=L"正在准备音频…";job=std::async(std::launch::async,[document,options]{return performance::render(document,options);});refresh();}
    void startRecord(){if(busy())return;if(mode->getSelectedIndex()!=0){message=L"步进模式直接演奏即可录入音符";refresh();return;}if(releaseInput)releaseInput();finishGesture();
        try{state.cursor=performance::snap(performance::ticks(parse(cursor->getText())),1<<grid->getSelectedIndex());recordStart=performance::beats(state.cursor);const double end=parse(rangeEnd->getText());if(end<=recordStart||end>20000)throw std::runtime_error("录制终点必须大于光标位置");recordLimit=end-recordStart;preBeats=preroll->getSelectedIndex()*4;recordBefore=state;if(documentRoot.empty())documentRoot=root();take.begin(state.document,instrument(),state.cursor,1<<grid->getSelectedIndex(),snapEnds->isChecked());
            performance::RenderOptions o;o.begin=recordStart-preBeats;o.end=end;o.tail=false;o.metronome=metronome->isChecked();launch(state.document,o,Recording);
        }catch(const std::exception& e){message=wide(e.what());refresh();}}
    void play(bool selectedClip,bool range){if(busy())return;if(releaseInput)releaseInput();finishGesture();step.finish(state,history);
        try{auto document=state.document;double begin=performance::beats(state.cursor),end=performance::beats(document.end());if(selectedClip){const auto* c=document.clip(state.clip);if(!c)throw std::runtime_error("先选择一个片段");performance::Track t;t.id=state.track;t.name="试听";t.clips={*c};t.clips[0].position=0;document.tracks={t};begin=0;end=performance::beats(c->length*c->repeats);}
            else if(range)end=parse(rangeEnd->getText());if(begin>=end)throw std::runtime_error("试听终点必须大于光标位置");restoreCursor=performance::beats(state.cursor);clipPreview=selectedClip;playbackBegin=begin;performance::RenderOptions o;o.begin=begin;o.end=end;launch(document,o,Playback);
        }catch(const std::exception& e){message=wide(e.what());refresh();}}
    void stopVoice(){if(valid()){if(voice)bound->stop(voice);if(playbackClip)bound->releaseClip(playbackClip);}voice=0;playbackClip=0;bound=nullptr;timeline->playhead=roll->playhead=-1;}
    void stop(){if(cancel)cancel->store(true);if(releaseInput)releaseInput();if(recording){releaseNotes();take.finish(state.document,elapsed());recording=false;if(!take.clip.notes.empty()){history.remember(recordBefore);auto* t=state.document.track(state.track);state.clip=take.clip.id;t->clips.push_back(std::move(take.clip));dirty=true;message=L"录制片段已放入轨道";}else{state=recordBefore;message=L"空录制未生成片段";}}else message=L"已停止 · 编辑光标保留";stopVoice();refresh();}
    void release(){if(cancel)cancel->store(true);recording=false;stopVoice();if(job.valid()){try{job.get();}catch(...){}}jobKind=None;}
    void focusLost(){if(recording||jobKind==Recording)stop();else if(voice)stop();releaseNotes();refresh();}
    void saveAsset(){if(busy())return;try{step.finish(state,history);const auto destination=chart::containedPath(root(),utf8(path->getText()));if(!documentRoot.empty()&&root()!=documentRoot)throw std::runtime_error("项目已切换，请返回原资产目录保存");const auto filename=chart::utf8(destination);if(!filename.ends_with(".performance.json"))throw std::runtime_error("文件名需以 .performance.json 结尾");if(std::filesystem::exists(destination)&&(filename!=savedFile||chart::read(destination)!=baseline))throw std::runtime_error("目标已存在或被外部修改，请使用新文件名");
            auto document=state.document;for(auto& t:document.tracks)for(auto& c:t.clips){auto& i=c.instrument;if(i.kind=="sample"&&!i.relative){const auto base=std::filesystem::weakly_canonical(std::filesystem::u8path(root())),source=std::filesystem::weakly_canonical(std::filesystem::u8path(i.source));const auto rel=source.lexically_relative(base);if(!rel.empty()&&!rel.is_absolute()&&*rel.begin()!=".."){i.source=chart::utf8(rel.generic_string());i.relative=true;chart::containedPath(root(),i.source);}}}
            const auto bytes=document.json().dump(2)+"\n";std::filesystem::create_directories(destination.parent_path());if(!ayt::io::File::atomicWrite(destination.string(),bytes.data(),bytes.size()))throw std::runtime_error("write failed");state.document=std::move(document);baseline=bytes;savedFile=filename;documentRoot=root();dirty=false;message=L"已保存编排";if(saved)saved(utf8(path->getText()));
        }catch(const std::exception& e){message=wide(e.what());}refresh();}
    bool loadFile(const std::filesystem::path& destination){if(busy())return false;try{const auto text=chart::read(destination);auto document=performance::Document::fromJson(nlohmann::json::parse(text));for(const auto& t:document.tracks)for(const auto& c:t.clips)if(c.instrument.kind=="sample"){const auto source=c.instrument.relative?chart::containedPath(root(),c.instrument.source):std::filesystem::u8path(c.instrument.source);if(!std::filesystem::is_regular_file(source))throw std::runtime_error("音源缺失："+c.instrument.source);}
            if(document.tracks.empty())document.addTrack();if(releaseInput)releaseInput();finishGesture();state={};state.document=std::move(document);state.track=state.document.tracks[0].id;if(!state.document.tracks[0].clips.empty())state.clip=state.document.tracks[0].clips[0].id;history={};step={};roll->selected.clear();baseline=text;savedFile=chart::utf8(destination);documentRoot=root();dirty=false;message=L"已打开编排";refresh();return true;
        }catch(const std::exception& e){message=wide(e.what());refresh();return false;}}
    void loadAsset(){try{loadFile(chart::containedPath(root(),utf8(path->getText())));}catch(const std::exception& e){message=wide(e.what());refresh();}}
    void exportWav(bool range=false){if(busy())return;try{step.finish(state,history);if(!documentRoot.empty()&&root()!=documentRoot)throw std::runtime_error("项目已切换，请返回原资产目录导出");auto logical=utf8(path->getText());if(logical.ends_with(".performance.json"))logical.resize(logical.size()-17);logical+="_"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count())+".wav";const auto destination=chart::containedPath(root(),logical);if(std::filesystem::exists(destination))throw std::runtime_error("WAV 已存在");exportFile=chart::utf8(destination);exportLogical=logical;performance::RenderOptions options;options.begin=performance::beats(state.cursor);options.end=range?parse(rangeEnd->getText()):performance::beats(state.document.end());if(!range)options.begin=0;if(options.end<=options.begin)throw std::runtime_error("导出终点必须大于光标位置");options.metronome=false;launch(state.document,options,Export);
        }catch(const std::exception& e){message=wide(e.what());refresh();}}
    void tick(){
        if(jobKind!=None&&job.valid()&&job.wait_for(std::chrono::seconds(0))==std::future_status::ready){const Job kind=jobKind;jobKind=None;
            try{auto result=job.get();if(cancel&&cancel->load())throw std::runtime_error("已取消");if(jobRoot!=root())throw std::runtime_error("准备期间项目已切换，请重试");
                if(kind==Export){const auto bytes=performance::wav(result);const auto destination=chart::containedPath(jobRoot,exportLogical);if(chart::utf8(destination)!=exportFile||std::filesystem::exists(destination))throw std::runtime_error("导出目标已变化");std::filesystem::create_directories(destination.parent_path());if(!ayt::io::File::atomicWrite(destination.string(),bytes.data(),bytes.size()))throw std::runtime_error("WAV write failed");message=L"已导出："+wide(exportLogical)+(result.peak>1?L" · 输出过载，请降低轨道音量":L"");if(saved)saved(exportLogical);}
                else{auto* e=engine();if(!e)throw std::runtime_error("音频引擎不可用");bound=e;generation=e->lifecycleGeneration();AudioClipDesc desc;desc.sampleRate=result.rate;desc.channels=2;desc.frameCount=uint32_t(result.pcm.size()/2);desc.pcmData=result.pcm.data();playbackClip=e->registerClip(desc);if(!playbackClip)throw std::runtime_error("播放资源注册失败");voice=e->play(playbackClip,AudioBus::Master);if(!voice){stopVoice();throw std::runtime_error("播放声部创建失败");}
                    if(kind==Recording){recording=true;recordEpoch=now();message=L"正在录制独立片段";}else message=L"正在试听 · 停止后编辑光标保留";}
            }catch(const std::exception& e){if(kind==Recording)state=recordBefore;stopVoice();message=wide(e.what());}refresh();
        }
        if(recording){const double since=now()-recordEpoch-preBeats*60/state.document.bpm;if(since>=recordLimit*60/state.document.bpm){stop();return;}message=since<0?L"预备拍 · "+number(-since*state.document.bpm/60,1)+L" 拍":L"录制中 · "+number(elapsed(),2)+L" s";timeline->playhead=recordStart+std::max(0.0,since)*state.document.bpm/60;timeline->markDirty();status->setText(message);}
        if(voice){if(!valid()){stop();message=L"音频引擎已更换，播放已停止";refresh();return;}VoiceRuntime voiceState;if(bound->voiceState(voice,voiceState)){if(!recording){const double beat=playbackBegin+double(voiceState.positionFrames)/bound->outputSampleRate()*state.document.bpm/60;timeline->playhead=clipPreview?-1:beat;roll->playhead=clipPreview?beat+(state.document.clip(state.clip)?performance::beats(state.document.clip(state.clip)->offset):0):-1;timeline->markDirty();roll->markDirty();}}else if(!recording){stopVoice();message=L"试听结束 · 编辑光标保留";refresh();}}
    }
};
