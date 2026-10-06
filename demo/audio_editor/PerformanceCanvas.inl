// Included in workbench. Timeline and piano roll share musical coordinates.
class PerformanceCanvas final : public LeafWidget {
public:
    performance::EditState* state=nullptr;performance::History* history=nullptr;
    bool roll=false,locked=false,dragging=false,panning=false,resizing=false,panMoved=false,panClick=false;
    double origin=0,visible=16,lastX=0,playhead=-1;
    int division=4,lowPitch=48,highPitch=72,firstTrack=0;
    std::vector<uint64_t> selected;
    std::function<void()> changed;std::function<bool()> additive;
    performance::EditState before;double anchor=0;int anchorPitch=0;uint64_t dragged=0;
    float x(double beat)const{const auto b=getWorldBounds();return b.minX+62+float((beat-origin)/visible)*std::max(1.f,b.maxX-b.minX-62);}
    double at(float px)const{const auto b=getWorldBounds();return std::max(0.0,origin+(px-b.minX-62)/std::max(1.f,b.maxX-b.minX-62)*visible);}
    performance::Tick quant(float px)const{return performance::snap(performance::ticks(std::min(at(px),19000.0)),division);}
    int pitch(float py)const{const auto b=getWorldBounds();return std::clamp(highPitch-int((py-b.minY-22)/std::max(1.f,(b.maxY-b.minY-22)/(highPitch-lowPitch+1))),0,127);}
    int trackIndex(float py)const{const auto b=getWorldBounds();return std::clamp(firstTrack+int((py-b.minY-22)/40),0,std::max(0,int(state->document.tracks.size())-1));}
    FRectangle noteBounds(const performance::Note& n)const{const auto b=getWorldBounds();const float h=(b.maxY-b.minY-22)/(highPitch-lowPitch+1),y=b.minY+22+(highPitch-n.pitch)*h;return {x(performance::beats(n.start)),y+1,std::max(x(performance::beats(n.start))+5,x(performance::beats(n.start+n.duration))),y+h-1};}
    bool onMouseButtonDown(const UIMouseEvent& e)override{
        if(!hitTest(e.mousePos))return false;const auto b=getWorldBounds();
        if(e.mouseButton==2||(e.mouseButton==0&&e.mousePos.y<b.minY+22&&e.mousePos.x>=b.minX+62)){panning=true;panMoved=false;panClick=e.mouseButton==0;lastX=e.mousePos.x;anchor=at(e.mousePos.x);return true;}
        if(locked)return false;
        before=*state;anchor=performance::beats(quant(e.mousePos.x));anchorPitch=pitch(e.mousePos.y);dragged=0;resizing=false;
        if(roll){auto* c=state->document.clip(state->clip);if(!c)return false;
            for(auto it=c->notes.rbegin();it!=c->notes.rend();++it)if(noteBounds(*it).contains(e.mousePos)){dragged=it->id;resizing=e.mousePos.x>noteBounds(*it).maxX-7;break;}
            if(e.mouseButton==1){if(dragged){history->remember(*state);c->notes.erase(std::remove_if(c->notes.begin(),c->notes.end(),[&](const auto& n){return n.id==dragged;}),c->notes.end());selected.clear();if(changed)changed();}return true;}
            if(e.mouseButton!=0)return false;
            if(dragged){if(additive&&additive()){if(std::find(selected.begin(),selected.end(),dragged)==selected.end())selected.push_back(dragged);else selected.erase(std::remove(selected.begin(),selected.end(),dragged),selected.end());}
                else if(std::find(selected.begin(),selected.end(),dragged)==selected.end())selected={dragged};}
            else{selected.clear();const auto start=quant(e.mousePos.x);performance::Note n{state->document.allocate(),anchorPitch,start,performance::PPQ/division,.8f,performance::beats(start),1.0/division};c->notes.push_back(n);dragged=n.id;selected={n.id};resizing=true;c->sourceLength=std::max(c->sourceLength,start+n.duration);c->length=std::max(c->length,c->sourceLength-c->offset);}
        }else{if(state->document.tracks.empty())return false;const int ti=trackIndex(e.mousePos.y);state->track=state->document.tracks[ti].id;
            for(auto it=state->document.tracks[ti].clips.rbegin();it!=state->document.tracks[ti].clips.rend();++it){const float left=x(performance::beats(it->position)),right=x(performance::beats(it->position+it->length*it->repeats));if(e.mousePos.x>=left&&e.mousePos.x<=right){dragged=it->id;resizing=it->repeats==1&&e.mousePos.x>right-7;break;}}
            state->clip=dragged;
            if(e.mouseButton==1){if(dragged){history->remember(before);auto& cs=state->document.tracks[ti].clips;cs.erase(std::remove_if(cs.begin(),cs.end(),[&](const auto& c){return c.id==dragged;}),cs.end());state->clip=0;}if(changed)changed();return true;}
            if(e.mousePos.x<b.minX+62){if(changed)changed();return true;}
            if(!dragged){panning=true;panMoved=false;panClick=true;lastX=e.mousePos.x;anchor=at(e.mousePos.x);state->clip=before.clip;if(changed)changed();return true;}
        }
        dragging=true;markDirty();if(changed)changed();return true;
    }
    bool onMouseMove(const UIMouseEvent& e)override{
        if(panning){if(std::abs(e.mousePos.x-lastX)>2)panMoved=true;origin=std::max(0.0,origin-(e.mousePos.x-lastX)/std::max(1.f,getSize().x-62)*visible);lastX=e.mousePos.x;markDirty();return true;}
        if(!dragging)return false;const auto delta=quant(e.mousePos.x)-performance::ticks(anchor);
        if(roll){auto* c=state->document.clip(state->clip);const auto* old=before.document.clip(state->clip);if(!c)return false;
            for(auto& n:c->notes)if(std::find(selected.begin(),selected.end(),n.id)!=selected.end()){
                const performance::Note* original=nullptr;if(old)for(const auto& candidate:old->notes)if(candidate.id==n.id){original=&candidate;break;}
                if(resizing){n.duration=std::max<performance::Tick>(performance::PPQ/division,quant(e.mousePos.x)-n.start);}
                else if(original){n.start=std::max<performance::Tick>(0,original->start+delta);n.pitch=std::clamp(original->pitch+pitch(e.mousePos.y)-anchorPitch,0,127);}
                n.rawStart=performance::beats(n.start);n.rawDuration=performance::beats(n.duration);c->sourceLength=std::max(c->sourceLength,n.start+n.duration);c->length=std::max(c->length,c->sourceLength-c->offset);
            }
        }else{auto* c=state->document.clip(dragged);const auto* old=before.document.clip(dragged);if(!c||!old)return false;
            if(resizing)c->length=std::clamp(old->length+delta,performance::PPQ/division,c->sourceLength-c->offset);
            else{c->position=std::max<performance::Tick>(0,old->position+delta);const uint64_t target=state->document.tracks[trackIndex(e.mousePos.y)].id;if(target!=state->track){performance::Clip moved=*c;auto* source=state->document.track(state->track);source->clips.erase(std::remove_if(source->clips.begin(),source->clips.end(),[&](const auto& v){return v.id==dragged;}),source->clips.end());state->document.track(target)->clips.push_back(std::move(moved));state->track=target;}}
        }markDirty();return true;
    }
    bool onMouseButtonUp(const UIMouseEvent&)override{if(panning){panning=false;if(panClick&&!panMoved&&!locked){state->cursor=performance::snap(performance::ticks(std::min(anchor,19000.0)),division);if(changed)changed();}markDirty();return true;}if(!dragging)return false;dragging=false;try{state->document.validate();if(state->document!=before.document)history->remember(before);}catch(...){*state=before;selected.clear();}markDirty();if(changed)changed();return true;}
    void onCaptureCancelled()override{if(dragging)*state=before;dragging=panning=false;markDirty();}
    bool onMouseWheel(const UIMouseWheelEvent& e)override{if(e.mousePos.x<getWorldBounds().minX+62){if(roll){lowPitch=std::clamp(lowPitch+(e.deltaY>0?-12:12),0,103);highPitch=lowPitch+24;}else firstTrack=std::clamp(firstTrack+(e.deltaY>0?1:-1),0,std::max(0,int(state->document.tracks.size())-1));markDirty();return true;}const double anchorBeat=at(e.mousePos.x);const double fraction=(e.mousePos.x-getWorldBounds().minX-62)/std::max(1.f,getSize().x-62);visible=std::clamp(visible*std::exp(e.deltaY*.008),1.0,512.0);origin=std::max(0.0,anchorBeat-visible*fraction);markDirty();return true;}
    void onRender(IRenderBackend& r)override{
        const auto b=getWorldBounds();r.drawRect(b,color("$color.bg.surface"));r.pushClip(b);const float left=b.minX+62;
        double step=1.0/division;while(step/visible*(b.maxX-left)<9)step*=2;
        for(double beat=std::ceil(origin/step)*step;beat<=origin+visible;beat+=step){const float px=x(beat);const bool bar=std::abs(beat/4-std::round(beat/4))<.0001;r.drawRect({px,b.minY+22,px+1,b.maxY},bar?FVector4{.32f,.35f,.42f,1}:FVector4{.18f,.20f,.24f,1});if(bar)r.drawText({px+2,b.minY,px+50,b.minY+20},std::to_wstring(int(beat/4)+1)+L"小节",10,color("$color.text.muted"));}
        if(roll){const auto* c=state->document.clip(state->clip);const float h=(b.maxY-b.minY-22)/(highPitch-lowPitch+1);
            for(int p=lowPitch;p<=highPitch;++p){const float y=b.minY+22+(highPitch-p)*h;r.drawRect({b.minX,y,left,y+h},black(p)?FVector4{.1f,.11f,.14f,1}:FVector4{.23f,.24f,.27f,1});if(p%12==0)r.drawText({b.minX+4,y,left,y+h},noteName(p),10,color("$color.text.primary"));r.drawRect({left,y,b.maxX,y+1},FVector4{.15f,.16f,.19f,1});}
            if(c){for(const auto& n:c->notes)if(n.pitch>=lowPitch&&n.pitch<=highPitch){const auto nb=noteBounds(n);if(nb.maxX<left||nb.minX>b.maxX)continue;r.drawRect({std::max(left,nb.minX),nb.minY,nb.maxX,nb.maxY},std::find(selected.begin(),selected.end(),n.id)!=selected.end()?FVector4{.95f,.7f,.22f,1}:color("$color.accent"));}
                bool pedal=false;performance::Tick start=0;for(const auto& p:c->pedals){if(p.down){start=p.time;pedal=true;}else if(pedal){r.drawRect({std::max(left,x(performance::beats(start))),b.maxY-4,x(performance::beats(p.time)),b.maxY},FVector4{.3f,.8f,.45f,1});pedal=false;}}}
            else r.drawText({left+12,b.minY+40,b.maxX,b.maxY},L"选择片段，或开启步进录入后演奏",13,color("$color.text.muted"));
        }else{for(int i=firstTrack;i<int(state->document.tracks.size())&&i<firstTrack+int((b.maxY-b.minY-22)/40)+1;++i){const auto& t=state->document.tracks[i];const float y=b.minY+22+(i-firstTrack)*40;
                r.drawRect({b.minX,y,left,y+39},t.id==state->track?FVector4{.2f,.3f,.4f,1}:FVector4{.16f,.17f,.2f,1});r.drawText({b.minX+3,y+5,left,y+35},std::filesystem::u8path(t.name).wstring(),11,color("$color.text.primary"));
                for(const auto& c:t.clips){const float a=x(performance::beats(c.position)),z=x(performance::beats(c.position+c.length*c.repeats));if(z<left||a>b.maxX)continue;r.drawRect({std::max(left,a),y+3,z,y+36},c.id==state->clip?FVector4{.65f,.46f,.14f,1}:FVector4{.14f,.33f,.5f,1});r.drawText({std::max(left,a)+4,y+7,z,y+30},std::filesystem::u8path(c.name).wstring()+L" ×"+std::to_wstring(c.repeats),11,color("$color.text.primary"));for(int k=1;k<c.repeats;++k){const float px=x(performance::beats(c.position+c.length*k));r.drawRect({px,y+3,px+1,y+36},FVector4{.5f,.6f,.7f,1});}}
            }}
        const double cursor=roll?(state->document.clip(state->clip)?performance::beats(std::max<performance::Tick>(0,state->cursor-state->document.clip(state->clip)->position+state->document.clip(state->clip)->offset)):0):performance::beats(state->cursor);
        for(const auto value:{cursor,playhead})if(value>=origin&&value<=origin+visible){const float px=x(value);r.drawRect({px,b.minY+22,px+1,b.maxY},value==playhead?FVector4{.3f,1,.5f,1}:color("$color.text.primary"));}r.popClip();
    }
};
