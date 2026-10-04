// Shared authoring chrome. Included by AudioEditorSession.cpp inside ayt::audio.
namespace workbench
{
using namespace ayt::ui;
using ayt::math::FRectangle;
using ayt::math::FVector4;
inline FVector4 color(const char *token)
{
    return expandColorToken(token);
}
inline int keyboardOffset(int key)
{
    constexpr int upper[] = {'A','W','S','E','D','F','T','G','Y','H','U','J','K'};
    constexpr int lower[] = {'Z','X','C','V','B','N','M',188};
    constexpr int offsets[] = {-12,-10,-8,-7,-5,-3,-1,0};
    for (int i=0;i<13;++i) if (key==upper[i]) return i;
    for (int i=0;i<8;++i) if (key==lower[i]) return offsets[i];
    return 128;
}
inline bool black(int n)
{
    const int k = n % 12;
    return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
}
inline std::wstring noteName(int n)
{
    static const wchar_t *names[] = {L"C", L"C#", L"D", L"D#", L"E", L"F", L"F#", L"G", L"G#", L"A", L"A#", L"B"};
    return names[(n % 12 + 12) % 12] + std::to_wstring(n / 12 - 1);
}
inline std::wstring number(double v, int precision = 2)
{
    wchar_t b[64];
    std::swprintf(b, 64, L"%.*f", precision, v);
    return b;
}
inline double parse(const std::wstring &text)
{
    size_t end = 0;
    const double v = std::stod(text, &end);
    if (end != text.size() || !std::isfinite(v))
        throw std::runtime_error("invalid number");
    return v;
}
inline TextLabel *label(const std::wstring &text, const std::string &id = {})
{
    auto *w = new TextLabel();
    w->setText(text);
    w->setId(id);
    w->setFontSize(13);
    return w;
}
inline HBox *row(VBox *parent, float height = 34)
{
    auto *w = new HBox();
    w->setSpacing(8);
    parent->addWidget(w, height);
    return w;
}
inline Button *button(HBox *parent, const std::wstring &text, const std::string &id, std::function<void()> fn,
                      float width = 110)
{
    auto *w = new Button();
    w->setId(id);
    w->setText(text);
    w->setOnClicked(std::move(fn));
    parent->addWidget(w, width);
    return w;
}
inline void placeholder(VBox *parent, const std::wstring &text)
{
    auto *r = row(parent);
    auto *b = button(r, text, "", [] {}, 240);
    b->setEnabled(false);
}
class Meter final : public LeafWidget
{
  public:
    std::wstring title=L"输出 L / R";
    float left = 0, right = 0, holdL = 0, holdR = 0, clipHold = 0, age = 0;
    uint64_t block = 0;
    void update(const AudioOutputLevels &v, float dt)
    {
        age = v.block == block ? age + dt : 0;
        block = v.block;
        const float l = age > .3f ? 0 : v.left, r = age > .3f ? 0 : v.right;
        left = std::max(l, left * std::exp(-dt * 12));
        right = std::max(r, right * std::exp(-dt * 12));
        holdL = std::max(l, holdL * std::exp(-dt * 1.8f));
        holdR = std::max(r, holdR * std::exp(-dt * 1.8f));
        clipHold = v.clipped && age == 0 ? 1.2f : std::max(0.0f, clipHold - dt);
        markDirty();
    }
    void onRender(IRenderBackend &renderer) override
    {
        const auto b = getWorldBounds();
        const float top=b.minY+24, bottom=b.maxY-12, height=std::max(1.0f,bottom-top);
        renderer.drawText({b.minX+6,b.minY,b.maxX,b.minY+20},title,12,color("$color.text.primary"));
        const auto normalized=[](float p) { return p>0 ? std::clamp((20*std::log10(p)+60)/60,0.0f,1.0f) : 0.0f; };
        for (int channel=0;channel<2;++channel) {
            const float value=channel?right:left, held=channel?holdR:holdL, x=b.minX+8+channel*20;
            renderer.drawRect({x,top,x+12,bottom},color("$color.bg.surface"));
            for (int segment=0;segment<20;++segment) if ((segment+.5f)/20<normalized(value)) {
                const auto c=segment>=18?FVector4{.95f,.25f,.2f,1}:segment>=16?FVector4{.92f,.7f,.2f,1}:FVector4{.2f,.72f,.44f,1};
                renderer.drawRect({x,bottom-height*(segment+1)/20+1,x+12,bottom-height*segment/20},c);
            }
            const float y=bottom-height*normalized(held);
            renderer.drawRect({x,y,x+12,std::min(bottom,y+2)},color("$color.text.primary"));
        }
        for (int db=0;db>=-60;db-=30) {
            const float y=top+height*(-db)/60;
            renderer.drawText({b.minX+46,y-6,b.minX+70,y+10},std::to_wstring(db),10,color("$color.text.muted"));
        }
        const float peak=std::max(holdL,holdR);
        renderer.drawText({b.minX+72,top,b.maxX,bottom},peak>=.001f?number(20*std::log10(peak),1)+L" dBFS":L"-inf dBFS",11,color("$color.text.muted"));
        if (clipHold>0) renderer.drawText({b.minX+72,bottom-14,b.maxX,b.maxY},L"过载",11,FVector4{.95f,.25f,.2f,1});
    }
};
class Piano final : public LeafWidget
{
  public:
    float width = 44, offset = 704, lastX = 0;
    bool dragging = false, playing = false;
    int mouseNote = -1, octave = 4, transpose = 0, low = 18, high = 102;
    std::function<void(int, bool)> onNote;
    std::function<bool(int)> isDown;
    FRectangle keyBounds(int note) const
    {
        const auto b = getWorldBounds();
        int white = 0;
        for (int n = 21; n < note; ++n)
            if (!black(n))
                ++white;
        const float x = b.minX + white * width - offset - (black(note) ? width * .31f : 0);
        return {x, b.minY + 30, x + width * (black(note) ? .62f : 1),
                b.minY + 30 + (b.maxY - b.minY - 30) * (black(note) ? .61f : 1)};
    }
    void clampOffset()
    {
        offset = std::clamp(offset, 0.0f, std::max(0.0f, 52 * width - getSize().x));
    }
    void center(int note)
    {
        offset = 0;
        offset = keyBounds(note).minX - getWorldBounds().minX - getSize().x * .28f;
        clampOffset();
        markDirty();
    }
    int noteAt(float x, float y) const
    {
        const auto b = getWorldBounds();
        if (x < b.minX || x > b.maxX || y < b.minY + 30 || y > b.maxY)
            return -1;
        for (int pass = 0; pass < 2; ++pass)
            for (int n = 21; n <= 108; ++n)
                if (black(n) == (pass == 0))
                {
                    const auto k = keyBounds(n);
                    if (x >= k.minX && x < k.maxX && y >= k.minY && y < k.maxY)
                        return n;
                }
        return -1;
    }
    void cancel()
    {
        const int released = mouseNote;
        mouseNote = -1;
        if (released >= 0 && onNote)
            onNote(released, false);
        playing = dragging = false;
        markDirty();
    }
    bool onMouseButtonDown(const UIMouseEvent &e) override
    {
        const auto b = getWorldBounds();
        if (!hitTest(e.mousePos) || (e.mouseButton != 0 && e.mouseButton != 2))
            return false;
        lastX = e.mousePos.x;
        if (e.mouseButton == 2 || e.mousePos.y < b.minY + 30)
            dragging = true;
        else
        {
            playing = true;
            mouseNote = noteAt(e.mousePos.x, e.mousePos.y);
            if (mouseNote >= 0 && onNote)
                onNote(mouseNote, true);
        }
        return true;
    }
    bool onMouseMove(const UIMouseEvent &e) override
    {
        if (dragging)
        {
            offset -= e.mousePos.x - lastX;
            lastX = e.mousePos.x;
            clampOffset();
            markDirty();
            return true;
        }
        if (playing)
        {
            const int n = noteAt(e.mousePos.x, e.mousePos.y);
            if (n != mouseNote)
            {
                if (mouseNote >= 0 && onNote)
                    onNote(mouseNote, false);
                mouseNote = n;
                if (n >= 0 && onNote)
                    onNote(n, true);
            }
            return true;
        }
        return false;
    }
    bool onMouseButtonUp(const UIMouseEvent &) override
    {
        if (!dragging && !playing)
            return false;
        cancel();
        return true;
    }
    bool onMouseWheel(const UIMouseWheelEvent &e) override
    {
        const auto b = getWorldBounds();
        const float x = e.mousePos.x - b.minX, anchor = (offset + x) / width;
        width = std::clamp(width * std::exp(-e.deltaY * .006f), 18.0f, 88.0f);
        offset = anchor * width - x;
        clampOffset();
        markDirty();
        return true;
    }
    void onCaptureCancelled() override
    {
        cancel();
    }
    void onRender(IRenderBackend &renderer) override
    {
        clampOffset();
        const auto b = getWorldBounds();
        renderer.drawRect(b, color("$color.bg.surface"));
        renderer.drawText({b.minX + 10, b.minY + 4, b.maxX - 8, b.minY + 26},
                          L"拖动此条平移音域  |  滚轮缩放键宽  |  中键平移", 12, color("$color.text.muted"));
        renderer.pushClip({b.minX, b.minY + 30, b.maxX, b.maxY});
        for (int pass = 0; pass < 2; ++pass)
            for (int n = 21; n <= 108; ++n)
                if (black(n) == (pass == 1))
                {
                    auto k = keyBounds(n);
                    if (k.maxX < b.minX || k.minX > b.maxX)
                        continue;
                    const bool supported = n + transpose >= low && n + transpose <= high,
                               down = isDown && isDown(n + transpose);
                    auto bg = down       ? FVector4{.25f, .55f, .9f, 1}
                              : black(n) ? FVector4{.10f, .12f, .16f, 1}
                                         : FVector4{.88f, .89f, .92f, 1};
                    if (!supported)
                        bg = {.35f, .36f, .39f, 1};
                    k.maxX -= 1;
                    renderer.drawRect(k, bg);
                    if (width >= 34)
                    {
                        const int index = n - (octave + 1) * 12;
                        static const wchar_t *shortcuts[] = {L"A", L"W", L"S", L"E", L"D", L"F", L"T",
                                                             L"G", L"Y", L"H", L"U", L"J", L"K"};
                        std::wstring shortcut=index>=0&&index<13?shortcuts[index]:L"";
                        constexpr int lowerOffsets[]={-12,-10,-8,-7,-5,-3,-1,0};
                        constexpr const wchar_t* lowerNames[]={L"Z",L"X",L"C",L"V",L"B",L"N",L"M",L","};
                        for(int i=0;i<8;++i) if(index==lowerOffsets[i]) shortcut+=(shortcut.empty()?L"":L"/")+std::wstring(lowerNames[i]);
                        const auto ink=black(n)?FVector4{.95f,.96f,.98f,1}:FVector4{.1f,.12f,.15f,1};
                        renderer.drawText({k.minX+3,k.maxY-38,k.maxX,k.maxY-20},noteName(n+transpose),11,ink);
                        renderer.drawText({k.minX+3,k.maxY-20,k.maxX,k.maxY-4},shortcut,11,ink);
                    }
                }
        renderer.popClip();
    }
};
class Waveform final : public LeafWidget
{
  public:
    std::vector<float> peaks;
    double duration = 0, begin = 0, end = 0, position = 0, visible = 0, origin = 0;
    int handle = 0;
    bool editableRange=true, showSelection=true;
    std::wstring emptyText=L"选择音频资源后显示真实波形";
    std::function<void(double, double, bool)> onRange;
    std::function<void(double)> onSeek;
    double time(float x) const
    {
        const auto b = getWorldBounds();
        return std::clamp(origin + (x - b.minX) / std::max(1.0f, b.maxX - b.minX) * (visible > 0 ? visible : duration),
                          0.0, duration);
    }
    float x(double t) const
    {
        const auto b = getWorldBounds();
        return b.minX +
               static_cast<float>((t - origin) / std::max(.001, visible > 0 ? visible : duration)) * (b.maxX - b.minX);
    }
    bool onMouseButtonDown(const UIMouseEvent &e) override
    {
        if (e.mouseButton != 0 || duration <= 0)
            return false;
        if (!editableRange) { if(onSeek) onSeek(time(e.mousePos.x)); return static_cast<bool>(onSeek); }
        handle = std::abs(e.mousePos.x - x(begin)) < 10 ? 1 : std::abs(e.mousePos.x - x(end)) < 10 ? 2 : 0;
        if (handle)
            return true;
        if (onSeek)
            onSeek(time(e.mousePos.x));
        return true;
    }
    bool onMouseMove(const UIMouseEvent &e) override
    {
        if (!handle)
            return false;
        const double t = time(e.mousePos.x);
        if (handle == 1)
            begin = std::clamp(t, 0.0, end - .001);
        else
            end = std::clamp(t, begin + .001, duration);
        if (onRange)
            onRange(begin, end, false);
        markDirty();
        return true;
    }
    bool onMouseButtonUp(const UIMouseEvent &) override
    {
        if (!handle)
            return false;
        handle = 0;
        if (onRange)
            onRange(begin, end, true);
        return true;
    }
    void onCaptureCancelled() override
    {
        onMouseButtonUp({{0, 0}});
    }
    bool onMouseWheel(const UIMouseWheelEvent &e) override
    {
        if (duration <= 0)
            return false;
        const double anchor = time(e.mousePos.x),
                     fraction = (e.mousePos.x - getWorldBounds().minX) / std::max(1.0f, getSize().x);
        visible = std::clamp((visible > 0 ? visible : duration) * std::exp(e.deltaY * .008), std::min(.05, duration),
                             duration);
        origin = std::clamp(anchor - visible * fraction, 0.0, duration - visible);
        markDirty();
        return true;
    }
    void onRender(IRenderBackend &renderer) override
    {
        const auto b = getWorldBounds();
        renderer.drawRect(b, color("$color.bg.surface"));
        if (duration <= 0)
        {
            renderer.drawText({b.minX + 20, b.minY + 70, b.maxX - 10, b.minY + 100}, emptyText, 14,
                              color("$color.text.muted"));
            return;
        }
        renderer.pushClip(b);
        const float center = (b.minY + b.maxY) / 2 + 10, height = (b.maxY - b.minY) * .35f;
        const auto accent = color("$color.accent");
        const auto background = color("$color.bg.surface");
        // GDI's solid rectangles ignore alpha. Precompose the selection tint
        // and paint it BELOW the waveform, so both native/GPU hosts agree.
        const FVector4 range{background.x*.84f+accent.x*.16f,
                             background.y*.84f+accent.y*.16f,
                             background.z*.84f+accent.z*.16f,1};
        const float left=std::max(b.minX,x(begin)),right=std::min(b.maxX,x(end));
        if(showSelection && right>left) renderer.drawRect({left,b.minY+24,right,b.maxY},range);
        renderer.drawRect({b.minX,center,b.maxX,center+1},color("$color.text.muted"));
        const float step=(b.maxX-b.minX)*static_cast<float>(duration/std::max(.001,visible>0?visible:duration))/std::max<size_t>(1,peaks.size());
        for (size_t i = 0; i < peaks.size(); ++i) {
            const float px = x(duration * (i+.5) / peaks.size());
            if (px < b.minX || px > b.maxX) continue;
            const float amplitude=std::clamp(peaks[i],0.f,1.f)*height;
            renderer.drawRect({px,center-std::max(.5f,amplitude),std::min(b.maxX,px+std::max(1.f,step-1)),center+std::max(.5f,amplitude)}, accent);
        }
        if(showSelection && x(begin)>=b.minX && x(begin)<=b.maxX)
            renderer.drawRect({x(begin),b.minY+24,std::min(b.maxX,x(begin)+2),b.maxY},accent);
        if(showSelection && x(end)>=b.minX && x(end)<=b.maxX)
            renderer.drawRect({std::max(b.minX,x(end)-2),b.minY+24,x(end),b.maxY},accent);
        if(x(position)>=b.minX && x(position)<=b.maxX)
            renderer.drawRect({x(position),b.minY+24,std::min(b.maxX,x(position)+1),b.maxY},color("$color.text.primary"));
        for (int i = 0; i <= 4; ++i)
        {
            const double t = origin + (visible > 0 ? visible : duration) * i / 4;
            const float px = b.minX + (b.maxX - b.minX) * i / 4;
            renderer.drawText({std::min(px, b.maxX - 55), b.minY + 3, std::min(px + 70, b.maxX), b.minY + 22},
                              number(t, 1) + L" s", 11, color("$color.text.muted"));
        }
        renderer.popClip();
    }
};
} // namespace workbench

struct AudioEditorSession::Workbench
{
    using VBox = ayt::ui::VBox;
    using HBox = ayt::ui::HBox;
    using TextInput = ayt::ui::TextInput;
    using Slider = ayt::ui::Slider;
    using Combo = ayt::ui::ComboBox;
    AudioEditorSession &s;
    ayt::ui::Widget *host = nullptr;
    VBox *root = nullptr;
    ayt::ui::TabControl *tabs = nullptr;
    workbench::Piano *piano = nullptr;
    workbench::Meter *meter = nullptr;
    workbench::Waveform *wave = nullptr;
    ayt::ui::TextLabel *instrument = nullptr, *resourceName = nullptr, *resourceStatus = nullptr, *diagnostic = nullptr,
                       *noteStatus = nullptr;
    ayt::ui::CheckBox *keyboardEnabled = nullptr, *loop = nullptr;
    ayt::ui::authoring::NumericFields *envelopeFields = nullptr;
    Combo *instrumentBus = nullptr, *resourceBus = nullptr;
    TextInput *transpose = nullptr;
    float attack = .004f, release = .14f, auditionGain = 1;
    int shift = 0;
    struct Gain
    {
        Slider *slider;
        TextInput *input;
        std::function<float()> read;
        std::function<void(float)> write;
        float scale;
    };
    std::vector<Gain> gains;
    AudioRecorder recorder;
    std::vector<RecordingDevice> recordingDevices;
    std::vector<std::string> inputIds, outputIds;
    Combo *recordSource=nullptr,*recordInput=nullptr,*recordOutput=nullptr;
    TextInput* recordPath=nullptr;
    workbench::Waveform* recordWave=nullptr;
    ayt::ui::TextLabel *recordStatus=nullptr,*recordLocation=nullptr;
    ayt::ui::CheckBox* recordMonitor=nullptr;
    ayt::ui::Button *recordStart=nullptr,*recordPause=nullptr,*recordStop=nullptr,*recordSave=nullptr,
                    *recordPlay=nullptr,*recordDiscard=nullptr,*recordEdit=nullptr,*recordRefresh=nullptr,*recordFolder=nullptr;
    bool devicesLoaded=false, recordSaved=false;
    std::string chosenRecordRoot,takeRoot,lastSavedRecording;
    std::wstring recordingMessage=L"就绪 · 输入设备包含已安装的虚拟声卡";
    uint64_t recordingMeterBlock=0;
    static std::wstring wide(const std::string& text) {
        try {return std::filesystem::u8path(text).wstring();}catch(...) {return L"设备或文件操作失败";}
    }
    static std::string utf8(const std::wstring& text) {
        const auto value=std::filesystem::path(text).u8string();return std::string(value.begin(),value.end());
    }
    static std::wstring recordingName() {
        const auto stamp=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        return L"audio/recordings/Take_"+std::to_wstring(stamp)+L".wav";
    }
    std::string recordingRoot() const {
        if(s._recordingRoot) return s._recordingRoot();
        if(!chosenRecordRoot.empty()) return chosenRecordRoot;
        const auto& project=ayt::project::Project::instance();
        return project.hasRoot()?project.resolve("Assets"):std::string{};
    }
    void refreshRecordingDevices() {
        const auto state=recorder.status();if(state.recording || state.playing) return;
        const auto oldInput=recordInput->getSelectedItem(),oldOutput=recordOutput->getSelectedItem();
        recorder.stopPlayback();
        std::string error;
        devicesLoaded=recorder.enumerateDevices(recordingDevices,&error);
        inputIds={""};outputIds={""};
        std::vector<std::wstring> inputs{L"系统默认输入"},outputs{L"系统默认输出"};
        for(const auto& device:recordingDevices) {
            auto& ids=device.input?inputIds:outputIds;auto& labels=device.input?inputs:outputs;
            ids.push_back(device.id);labels.push_back(wide(device.name)+(device.isDefault?L" · 默认":L""));
        }
        recordInput->setItems(inputs);recordOutput->setItems(outputs);
        auto restore=[](Combo* combo,const std::vector<std::wstring>& labels,const std::wstring& old) {
            const auto found=std::find(labels.begin(),labels.end(),old);
            combo->setSelectedIndex(found==labels.end()?0:static_cast<int>(found-labels.begin()));
        };
        restore(recordInput,inputs,oldInput);restore(recordOutput,outputs,oldOutput);
        recordingMessage=devicesLoaded?L"设备已刷新 · 48 kHz / 立体声 · 最长 10 分钟":wide(error);
    }
    void startRecording() {
        const auto state=recorder.status();if(state.recording || (state.frames && !recordSaved)) return;
        if(!devicesLoaded) refreshRecordingDevices();
        RecordingConfig config;
        const int input=recordInput->getSelectedIndex(),output=recordOutput->getSelectedIndex();
        if(input<0 || output<0 || input>=static_cast<int>(inputIds.size()) || output>=static_cast<int>(outputIds.size())) return;
        config.inputDevice=inputIds[input];config.outputDevice=outputIds[output];
        config.loopback=recordSource->getSelectedIndex()==1;
        config.monitor=recordMonitor->isChecked() && !config.loopback;
        std::string error;
        if(!recorder.start(config,&error)) {recordingMessage=L"录音启动失败："+wide(error);return;}
        if(recordSaved) recordPath->setText(recordingName());
        recordSaved=false;lastSavedRecording.clear();takeRoot=recordingRoot();
        recordWave->origin=recordWave->visible=0;
        recordingMessage=config.loopback?L"正在录制所选输出设备的全部声音":L"正在录制所选输入设备";
    }
    void saveRecording() {
        const auto root=recordingRoot();
        if(!takeRoot.empty() && root!=takeRoot) {recordingMessage=L"项目已切换：请返回录音时的项目后保存";return;}
        std::string error;
        try {
            const auto logical=utf8(recordPath->getText());
            if(!recorder.saveWav(root,logical,&error)) {recordingMessage=L"保存失败："+wide(error);return;}
            lastSavedRecording=(std::filesystem::u8path(root)/std::filesystem::u8path(logical)).string();
            recordSaved=true;recordingMessage=L"已保存："+recordPath->getText();
            if(s._recordingSaved) s._recordingSaved(logical);
        }catch(const std::exception& e) {recordingMessage=L"保存失败："+wide(e.what());}
    }
    void buildRecordingPage() {
        using namespace workbench;
        auto* page=new VBox();page->setPadding(12,14,12,8);page->setSpacing(8);
        addPage(L"录音台",page);
        auto* source=row(page,38);source->addWidget(label(L"录音来源"),140);
        recordSource=new Combo();recordSource->setId("recording_source");
        recordSource->setItems({L"输入设备 / 虚拟声卡",L"输出回环 · 系统声音"});recordSource->setSelectedIndex(0);source->addWidget(recordSource);
        recordRefresh=button(source,L"刷新设备","recording_refresh",[this]{refreshRecordingDevices();},100);
        auto deviceRow=[&](const std::wstring& name,const char* id,Combo*& combo,const std::wstring& initial) {
            auto* r=row(page,38);r->addWidget(label(name),140);combo=new Combo();combo->setId(id);
            combo->setItems({initial});combo->setSelectedIndex(0);r->addWidget(combo);
        };
        deviceRow(L"音频输入","recording_input",recordInput,L"系统默认输入");
        deviceRow(L"监听 / 回放输出","recording_output",recordOutput,L"系统默认输出");
        recordSource->setOnSelectionChanged([this](int index){if(index==1) recordMonitor->setChecked(false);});
        auto* monitoring=row(page,30);
        recordMonitor=new ayt::ui::CheckBox();recordMonitor->setId("recording_monitor");recordMonitor->setText(L"实时监听输入（建议使用耳机）");recordMonitor->setChecked(false);
        monitoring->addWidget(recordMonitor,300);
        monitoring->addWidget(label(L"输出回环不启用监听 · 输出选择仅用于本录音台"));
        auto* location=row(page,30);recordLocation=label(L"", "recording_location");location->addWidget(recordLocation);
        recordFolder=button(location,L"选择资产目录…","recording_folder",[this]{
            if(s._recordingFolderPicker) {auto root=s._recordingFolderPicker();if(!root.empty()) chosenRecordRoot=std::move(root);}
        },130);
        auto* pathRow=row(page,38);pathRow->addWidget(label(L"项目虚拟路径"),140);
        recordPath=new TextInput();recordPath->setId("recording_path");recordPath->setMaxLength(240);recordPath->setText(recordingName());pathRow->addWidget(recordPath);
        auto* transport=row(page,38);
        recordStart=button(transport,L"开始录音","recording_start",[this]{startRecording();},105);
        recordPause=button(transport,L"暂停","recording_pause",[this]{const bool paused=!recorder.status().paused;recorder.pause(paused);recordingMessage=paused?L"录音已暂停":L"录音继续";},72);
        recordStop=button(transport,L"停止","recording_stop",[this]{recorder.stop();recorder.stopPlayback();recordingMessage=L"已停止 · 保存后可在音频资源中编辑";},72);
        recordPlay=button(transport,L"回放录音","recording_play",[this]{
            const int index=recordOutput->getSelectedIndex();std::string error;
            if(index>=0 && index<static_cast<int>(outputIds.size()) && recorder.play(outputIds[index],&error)) recordingMessage=L"正在回放录音";
            else recordingMessage=L"回放失败："+wide(error);
        },100);
        recordSave=button(transport,L"保存 WAV","recording_save",[this]{saveRecording();},100);
        recordDiscard=button(transport,L"丢弃录音","recording_discard",[this]{
            if(recorder.clear()) {recordSaved=false;lastSavedRecording.clear();recordPath->setText(recordingName());recordingMessage=L"录音已清除 · 已保存文件保留";}
        },100);
        recordWave=new Waveform();recordWave->setId("recording_waveform");recordWave->editableRange=false;recordWave->showSelection=false;
        recordWave->emptyText=L"点击开始录音，显示实时波形；停止后可以缩放查看";page->addWidget(recordWave,220);
        recordStatus=label(L"就绪","recording_status");page->addWidget(recordStatus,32);
        auto* actions=row(page,34);
        recordEdit=button(actions,L"在音频资源中编辑","recording_edit",[this]{
            if(!lastSavedRecording.empty() && loadResource(lastSavedRecording)) tabs->setSelectedIndex(1);
        },175);
        actions->addWidget(label(L"WAV · 48 kHz / 16-bit · 滚轮缩放波形"));
        page->addWidget(label(L"未保存录音需先保存或丢弃再开始新录音；关闭窗口会停止录音并释放设备。"),28);
        inputIds={""};outputIds={""};
    }
    void tickRecording() {
        recorder.update();const auto state=recorder.status();const bool busy=state.recording || state.playing;
        recordSource->setEnabled(!busy);recordInput->setEnabled(!busy && recordSource->getSelectedIndex()==0);
        recordOutput->setEnabled(!busy);recordRefresh->setEnabled(!busy);
        recordMonitor->setEnabled(!busy && recordSource->getSelectedIndex()==0);
        recordFolder->setEnabled(!busy && static_cast<bool>(s._recordingFolderPicker));
        recordPath->setReadOnly(busy);
        recordStart->setEnabled(!busy && (!state.frames || recordSaved));
        recordPause->setEnabled(state.recording);recordPause->setText(state.paused?L"继续":L"暂停");
        recordStop->setEnabled(busy);recordSave->setEnabled(!busy && state.frames && !recordSaved && !recordingRoot().empty());
        recordDiscard->setEnabled(!state.recording && state.frames>0);recordPlay->setEnabled(!busy && state.frames>0);
        recordEdit->setEnabled(!busy && recordSaved);
        const auto root=recordingRoot();recordLocation->setText(root.empty()?L"尚未选择项目资产目录 · 录音保留在内存":L"保存位置：项目资产目录 / "+recordPath->getText());
        const double seconds=static_cast<double>(state.frames)/state.sampleRate;
        if(recordWave->duration!=seconds) {
            recordWave->duration=seconds;recordWave->begin=0;recordWave->end=seconds;
            recordWave->peaks=recorder.waveform(1024);
            if(state.recording) recordWave->origin=recordWave->visible=0;
            recordWave->markDirty();
        }
        recordWave->position=state.playing?state.positionSeconds:seconds;recordWave->markDirty();
        auto message=state.limitReached?L"已达到 10 分钟上限 · 录音自动停止，请保存":recordingMessage;
        message+=L" · "+workbench::number(seconds,2)+L" s";
        if(state.recording) message+=L" · 输入 "+(state.peak>.001f?workbench::number(20*std::log10(state.peak),1)+L" dBFS":L"-inf dBFS");
        if(state.droppedFrames) message+=L" · 丢帧 "+std::to_wstring(state.droppedFrames)+L"（录音不完整）";
        recordStatus->setText(message);
    }

    struct Edit
    {
        double begin = 0, end = 0;
        float gain = 1, fadeIn = 0, fadeOut = 0;
    };
    Edit edit, baseline, saved;
    std::vector<Edit> undo, redo;
    std::array<TextInput *, 5> fields{};
    std::shared_ptr<ayt::resource::Audio> resource;
    AudioClipId resourceClip = 0;
    VoiceHandle resourceVoice = 0;
    AudioEngine *resourceEngine = nullptr;
    uint64_t resourceGeneration = 0;
    std::string path;
    double duration = 0;
    bool resourcePaused = false, resourceWritable = true, updating = false, rangeDragging = false;
    nlohmann::json sidecar;
    static AudioBus bus(int index)
    {
        static constexpr AudioBus values[] = {AudioBus::Sfx, AudioBus::Music, AudioBus::Ui, AudioBus::Voice};
        return values[std::clamp(index, 0, 3)];
    }
    Gain addGain(VBox *parent, const std::wstring &name, const std::string &id, float min, float max, float value,
                 std::function<float()> read, std::function<void(float)> write, float scale = 100)
    {
        using namespace workbench;
        auto *r = row(parent, 38);
        r->addWidget(label(name), 140);
        auto *slider = new Slider();
        slider->setId("sld_" + id);
        slider->setValueRange(min, max);
        slider->setValue(value);
        r->addWidget(slider);
        auto *input = new TextInput();
        input->setId("val_" + id);
        input->setText(number(value, max <= 2 ? 2 : 0));
        input->setMaxLength(12);
        r->addWidget(input, 74);
        r->addWidget(label(max <= 2 ? L"×" : max > 200 ? L"ms" : L"%"), 30);
        Gain g{slider, input, read, write, scale};
        gains.push_back(g);
        slider->setOnValueChanged([this, input, write, scale, max](float v) {
            if (updating)
                return;
            input->setText(workbench::number(v, max <= 2 ? 2 : 0));
            write(v / scale);
        });
        auto commit = [this, slider, input, read, write, min, max, scale]() {
            if (updating)
                return;
            try
            {
                const float v = std::clamp(static_cast<float>(workbench::parse(input->getText())), min, max);
                slider->setValue(v);
                write(v / scale);
                input->setText(workbench::number(v, max <= 2 ? 2 : 0));
            }
            catch (...)
            {
                input->setText(workbench::number(read() * scale, max <= 2 ? 2 : 0));
            }
        };
        input->setOnSubmit([commit](const std::wstring &) { commit(); });
        input->setOnFocusLostNotify(commit);
        return g;
    }
    Combo *route(HBox *r, const std::string &id, std::function<void(int)> callback)
    {
        auto *c = new Combo();
        c->setId(id);
        c->setItems({L"音效 · SFX", L"音乐 · Music", L"界面音 · UI", L"语音 · Voice"});
        c->setSelectedIndex(0);
        c->setOnSelectionChanged(std::move(callback));
        r->addWidget(c, 200);
        return c;
    }
    void addPage(const std::wstring &title, VBox *page)
    {
        auto *scroll = new ayt::ui::ScrollView();
        scroll->setHorizontalScrollBarEnabled(false);
        scroll->setContentOwned(page);
        tabs->addTabOwned(title, scroll);
    }
    Workbench(AudioEditorSession &owner, ayt::ui::Widget *mount) : s(owner), host(mount)
    {
        using namespace workbench;
        root = new VBox();
        root->setId("audio_workbench");
        root->setPadding(18, 14, 14, 12);
        root->setSpacing(12);
        ayt::ui::AnchorLayout a;
        a.anchorMax = {1, 1};
        root->setLayoutPositionManaged(false);
        root->setLayoutSizeManaged(false);
        root->setAnchorLayout(a);
        host->addChild(root);
        auto *heading = row(root, 80);
        heading->addWidget(label(L"AYAudio · 音频工作台"), 210);
        s._status = label(L"音源准备中", "lbl_status");
        heading->addWidget(s._status);
        meter = new Meter();
        meter->setId("audio_output_meter");
        heading->addWidget(meter, 150);
        auto *content = new HBox();
        content->setSpacing(16);
        root->addWidget(content);
        tabs = new ayt::ui::TabControl();
        tabs->setId("audio_tabs");
        tabs->setHeaderHeight(38);
        tabs->getTabStrip()->setFlatHeader(true);
        tabs->getTabStrip()->setMinTabWidth(112);
        tabs->getTabStrip()->setIndicatorTweenMs(0);
        content->addWidget(tabs);

        auto *performance = new VBox();
        performance->setPadding(12, 14, 12, 8);
        performance->setSpacing(4);
        addPage(L"音源演奏", performance);
        auto *p = row(performance);
        p->addWidget(label(L"演奏参数"), 78);
        auto *preset = new Combo();
        preset->setId("instrument_preset");
        preset->setItems({L"默认演奏", L"柔和演奏"});
        preset->setSelectedIndex(0);
        preset->setOnSelectionChanged([this](int index) {
            s.releaseKeys();
            s._previewGain = index ? .65f : .7f;
            attack = index ? .012f : .004f;
            release = index ? .30f : .14f;
            if (envelopeFields)
                envelopeFields->setValues({attack * 1000, release * 1000});
            applyInstrument();
        });
        p->addWidget(preset, 180);
        auto *save = button(p, L"保存预设（待实现）", "instrument_preset_save", [] {}, 190);
        save->setEnabled(false);
        instrument = label(L"Salamander Grand Piano", "lbl_instrument");
        performance->addWidget(instrument, 28);
        auto *source = row(performance);
        button(
            source, L"钢琴 / 正弦波", "btn_instrument",
            [this] {
                if (s.ensureClips())
                    s.buildInstrument(!s._sine);
                s.updateKeys();
            },
            145);
        button(source, L"导入采样…", "btn_load_sample", [this] { s.loadSample(); }, 120);
        source->addWidget(label(L"C1–C7 根音采样 · 实际音域按采样映射"));
        addGain(
            performance, L"演奏音量", "preview", 0, 100, 70, [this] { return s._previewGain; },
            [this](float v) {
                s._previewGain = v;
                applyInstrument();
            });
        addGain(
            performance, L"音符力度", "velocity", 1, 100, 80, [this] { return s._velocity; },
            [this](float v) { s._velocity = v; });
        auto *envelope = row(performance);
        envelope->addWidget(label(L"起音 / 释放"), 140);
        envelopeFields =
            new ayt::ui::authoring::NumericFields({"instrument_attack", "instrument_release"}, {L"起音", L"释放"});
        envelopeFields->setValues({attack * 1000, release * 1000});
        envelopeFields->setUnit(L"ms");
        envelopeFields->setOnSubmitted([this] {
            std::vector<float> v;
            if (envelopeFields->readValues(2, v) && v[0] >= 0 && v[0] <= 2000 && v[1] >= 0 && v[1] <= 5000)
            {
                attack = v[0] / 1000;
                release = v[1] / 1000;
                applyInstrument();
            }
            else
                envelopeFields->setValues({attack * 1000, release * 1000});
        });
        envelope->addWidget(envelopeFields);
        auto *routing = row(performance);
        routing->addWidget(label(L"演奏输出到"), 140);
        instrumentBus = route(routing, "instrument_bus", [this](int) {
            s.releaseKeys();
            applyInstrument();
        });
        routing->addWidget(label(L"所选分组 → 总输出"));
        auto *toolbar = row(performance, 36);
        button(
            toolbar, L"音域 −", "btn_octave_down",
            [this] {
                s.releaseKeys();
                s._octave = std::max(1, s._octave - 1);
                s.updateKeys();
                piano->center((s._octave + 1) * 12);
            },
            78);
        button(
            toolbar, L"音域 +", "btn_octave_up",
            [this] {
                s.releaseKeys();
                s._octave = std::min(6, s._octave + 1);
                s.updateKeys();
                piano->center((s._octave + 1) * 12);
            },
            78);
        toolbar->addWidget(label(L"移调"), 40);
        button(toolbar, L"−", "transpose_down", [this] { setShift(shift - 1); }, 32);
        transpose = new TextInput();
        transpose->setId("transpose_value");
        transpose->setText(L"0");
        transpose->setMaxLength(4);
        toolbar->addWidget(transpose, 48);
        button(toolbar, L"+", "transpose_up", [this] { setShift(shift + 1); }, 32);
        toolbar->addWidget(label(L"半音"), 40);
        auto commitShift = [this] {
            try
            {
                const double v = parse(transpose->getText());
                if (v != std::floor(v))
                    throw std::runtime_error("integer required");
                setShift(static_cast<int>(std::clamp(v, -24.0, 24.0)));
            }
            catch (...)
            {
                setShift(shift);
            }
        };
        transpose->setOnSubmit([commitShift](const std::wstring &) { commitShift(); });
        transpose->setOnFocusLostNotify(commitShift);
        auto *keys = row(performance, 34);
        keyboardEnabled = new ayt::ui::CheckBox();
        keyboardEnabled->setId("keyboard_enabled");
        keyboardEnabled->setText(L"电脑键盘演奏");
        keyboardEnabled->setChecked(true);
        keyboardEnabled->setOnToggled([this](bool) { s.releaseKeys(); });
        keys->addWidget(keyboardEnabled, 160);
        button(
            keys, L"延音关闭", "btn_sustain",
            [this] {
                if (!s.ensureClips())
                    return;
                s._pedalLatch = !s._pedalLatch;
                s._instrument->setSustain(s._spaceDown || s._pedalLatch);
                s.updateKeys();
            },
            110);
        button(keys, L"释放全部音符", "btn_stop_all", [this] { s.releaseKeys(); }, 140);
        keys->addWidget(label(L"Space：延音"));
        performance->addWidget(label(L"左 Shift：临时升一个八度  |  左 Alt：临时降一个八度  |  同时按住：抵消"), 24);
        performance->addWidget(label(L"下排 Z X C V B N M ,：低八度白键（C 到 C）"), 24);
        piano = new Piano();
        piano->setId("piano_keyboard");
        piano->onNote = [this](int n, bool down) {
            if (down) { s._mouseKey=n+s.effectiveTranspose();s.pressPitch(s._mouseKey,false,true); }
            else if (s._mouseKey>=0) { const int pitch=s._mouseKey;s._mouseKey=-1;s.pressPitch(pitch,false,false); }
        };
        piano->isDown = [this](int n) { return n >= 0 && n < 128 && (s._keyboard[n] || s._mouse[n]); };
        performance->addWidget(piano, 160);
        noteStatus = label(L"A W S E D F T G Y H U J K · 缩放和平移保持音高", "note_status");
        performance->addWidget(noteStatus, 28);
        auto *resources = new VBox();
        resources->setPadding(12, 14, 12, 8);
        resources->setSpacing(4);
        addPage(L"音频资源", resources);
        auto *open = row(resources);
        button(
            open, L"选择资源…", "resource_open",
            [this] {
                if (s._pathPicker)
                {
                    s.releaseKeys();
                    const auto p = s._pathPicker();
                    if (!p.empty())
                        loadResource(p);
                }
            },
            125);
        resourceName = label(L"未选择音频资源", "resource_name");
        open->addWidget(resourceName);
        auto *rr = row(resources);
        rr->addWidget(label(L"试听输出到"), 140);
        resourceBus = route(rr, "resource_bus", [this](int) { stopResource(); });
        addGain(
            resources, L"试听音量", "audition", 0, 150, 100, [this] { return auditionGain; },
            [this](float v) {
                auditionGain = v;
                updateResourceGain();
            });
        wave = new Waveform();
        wave->setId("resource_waveform");
        wave->onSeek = [this](double t) {
            wave->position = t;
            if (resourceValid() && resourceVoice)
                s.liveEngine()->setVoicePositionFrames(resourceVoice,
                                                       static_cast<uint64_t>(t * resource->getSampleRate()));
            wave->markDirty();
        };
        wave->onRange = [this](double a, double b, bool commit) {
            if (!resourceWritable)
            {
                syncResource();
                return;
            }
            if (!rangeDragging)
            {
                baseline = edit;
                rangeDragging = true;
            }
            edit.begin = a;
            edit.end = b;
            edit.fadeIn = std::min(edit.fadeIn, static_cast<float>(b - a));
            edit.fadeOut = std::min(edit.fadeOut, static_cast<float>(b - a));
            if (commit)
            {
                rangeDragging = false;
                commitEdit(baseline);
            }
            syncResource();
        };
        resources->addWidget(wave, 160);
        resources->addWidget(label(L"拖动边界调整区间 · 点击定位 · 滚轮缩放波形"), 24);
        auto *editRow = row(resources, 58);
        const wchar_t *names[] = {L"起点 / s", L"终点 / s", L"片段增益 / %", L"淡入 / s", L"淡出 / s"};
        const char *ids[] = {"range_begin", "range_end", "clip_gain", "fade_in", "fade_out"};
        for (int i = 0; i < 5; ++i)
        {
            auto *column = new VBox();
            column->setSpacing(4);
            column->addWidget(label(names[i]), 20);
            fields[i] = new TextInput();
            fields[i]->setId(ids[i]);
            column->addWidget(fields[i], 30);
            editRow->addWidget(column);
            fields[i]->setOnSubmit([this](const std::wstring &) { applyResourceFields(); });
            fields[i]->setOnFocusLostNotify([this] { applyResourceFields(); });
        }
        auto *transport = row(resources);
        button(transport, L"试听区间", "resource_play", [this] { playResource(); }, 100);
        button(
            transport, L"暂停 / 恢复", "resource_pause",
            [this] {
                if (resourceValid() && resourceVoice)
                {
                    resourcePaused = !resourcePaused;
                    s.liveEngine()->setVoicePaused(resourceVoice, resourcePaused);
                    resourceStatus->setText(resourcePaused ? L"试听已暂停 · 琴键可继续演奏" : L"资源试听中");
                }
            },
            120);
        button(transport, L"停止试听", "resource_stop", [this] { stopResource(); }, 100);
        loop = new ayt::ui::CheckBox();
        loop->setText(L"循环");
        loop->setChecked(true);
        transport->addWidget(loop, 80);
        auto *commands = row(resources);
        button(
            commands, L"撤销", "resource_undo",
            [this] {
                if (!undo.empty())
                {
                    redo.push_back(edit);
                    edit = undo.back();
                    undo.pop_back();
                    stopResource();
                    syncResource();
                }
            },
            72);
        button(
            commands, L"重做", "resource_redo",
            [this] {
                if (!redo.empty())
                {
                    undo.push_back(edit);
                    edit = redo.back();
                    redo.pop_back();
                    stopResource();
                    syncResource();
                }
            },
            72);
        button(commands, L"保存编辑信息", "resource_save", [this] { saveResource(); }, 145);
        auto *exportButton = button(commands, L"导出音频（待实现）", "resource_export", [] {}, 185);
        exportButton->setEnabled(false);
        placeholder(resources, L"音量包络编辑（待实现）");
        resourceStatus = label(L"保存编辑信息不改写原始音频", "resource_status");
        resources->addWidget(resourceStatus, 36);
        auto *mixer = new VBox();
        mixer->setPadding(12, 14, 12, 8);
        mixer->setSpacing(12);
        addPage(L"混音器", mixer);
        mixer->addWidget(label(L"当前引擎混音 · 实时生效 · 本次会话"), 26);
        mixer->addWidget(label(L"影响共享引擎的演奏、资源试听与场景声音；不会自动保存为项目配置"), 24);
        auto *mixActions = row(mixer);
        button(
            mixActions, L"暂停引擎输出", "engine_pause",
            [this] {
                if (auto *e = s.liveEngine())
                    e->pause();
            },
            150);
        button(
            mixActions, L"恢复输出", "engine_resume",
            [this] {
                if (auto *e = s.liveEngine())
                    e->resume();
            },
            110);
        mixer->addWidget(label(L"100% = 原始增益 · 0% = 静音 · 大于 100% 放大信号"), 24);
        addGain(
            mixer, L"总输出 · Master", "master", 0, 150, 100,
            [this] {
                auto *e = s.liveEngine();
                return e ? e->masterGain() : 1;
            },
            [this](float v) {
                if (auto *e = s.liveEngine())
                    e->setMasterGain(v);
            });
        const AudioBus busIds[] = {AudioBus::Music, AudioBus::Sfx, AudioBus::Ui, AudioBus::Voice};
        const wchar_t *busLabels[] = {L"音乐 · Music", L"音效 · SFX", L"界面音 · UI", L"语音 · Voice"};
        const char *busStrings[] = {"music", "sfx", "ui", "voice"};
        for (int i = 0; i < 4; ++i)
        {
            const auto b = busIds[i];
            addGain(
                mixer, busLabels[i], busStrings[i], 0, 150, 100,
                [this, b] {
                    auto *e = s.liveEngine();
                    return e ? e->busGain(b) : 1;
                },
                [this, b](float v) {
                    if (auto *e = s.liveEngine())
                        e->setBusGain(b, v);
                });
        }
        mixer->addWidget(label(L"效果 · 仅影响送入混响的声音"), 28);
        addGain(
            mixer, L"混响量", "reverb", 0, 100, 25,
            [this] {
                auto *e = s.liveEngine();
                return e ? e->reverbReturn() : .25f;
            },
            [this](float v) {
                if (auto *e = s.liveEngine())
                    e->setMasterReverbReturn(v);
            });
        placeholder(mixer, L"独立编辑器试听分组（待实现）");
        placeholder(mixer, L"另存项目混音预设（待实现）");
        buildRecordingPage();
        diagnostic = label(L"诊断：等待音频引擎", "audio_diagnostic");
        root->addWidget(diagnostic, 28);
        tabs->setOnSelectionChanged([this](int index) {
            if(index==3 && !devicesLoaded) refreshRecordingDevices();
            s.releaseKeys();
            s._ui->invalidateLayout();
            s._ui->layout();
        });
    }
    ~Workbench()
    {
        recorder.stop();
        recorder.stopPlayback();
        releaseAudio();
        if (host && root)
        {
            host->removeChild(root);
            ayt::ui::destroyWidgetTree(root);
        }
    }
    void applyInstrument()
    {
        if (s._instrument)
        {
            s._instrument->setGain(s._previewGain);
            s._instrument->setEnvelope(attack, release);
            if (instrumentBus)
                s._instrument->setBus(bus(instrumentBus->getSelectedIndex()));
        }
    }
    void setShift(int value)
    {
        s.releaseKeys();
        shift = std::clamp(value, -24, 24);
        transpose->setText((shift > 0 ? L"+" : L"") + std::to_wstring(shift));
        s.updateKeys();
    }
    bool resourceValid() const
    {
        auto *e = s.liveEngine();
        return e && e == resourceEngine && e->lifecycleGeneration() == resourceGeneration;
    }
    void stopResource()
    {
        if (resourceValid() && resourceVoice)
            s.liveEngine()->stop(resourceVoice);
        resourceVoice = 0;
        resourcePaused = false;
        if (resourceStatus)
            resourceStatus->setText(L"资源试听已停止");
    }
    void releaseAudio()
    {
        stopResource();
        if (resourceValid() && resourceClip)
            s.liveEngine()->releaseClip(resourceClip);
        resourceClip = 0;
        resourceEngine = nullptr;
    }
    bool loadResource(const std::string &value)
    {
        if (!s.ensureClips())
            return false;
        ayt::resource::AudioLoader loader;
        auto next = std::dynamic_pointer_cast<ayt::resource::Audio>(loader.load(value));
        if (!next || !next->getChannels() || !next->getSampleRate() ||
            (next->getBitsPerSample() != 16 && next->getBitsPerSample() != 32))
        {
            resourceStatus->setText(L"无法解码音频资源");
            return false;
        }
        const auto c = s._audio->registerClipFromResource(next.get());
        if (!c)
        {
            resourceStatus->setText(L"资源注册失败");
            return false;
        }
        releaseAudio();
        resource = next;
        resourceClip = c;
        resourceEngine = s.liveEngine();
        resourceGeneration = resourceEngine->lifecycleGeneration();
        path = value;
        const size_t frames = next->getDataSize() / (next->getChannels() * (next->getBitsPerSample() / 8));
        duration = static_cast<double>(frames) / next->getSampleRate();
        edit = {0, duration, 1, 0, 0};
        resourceWritable = true;
        sidecar = nlohmann::json::object();
        undo.clear();
        redo.clear();
        const auto metadata = path + ".timeline.json";
        if (std::filesystem::exists(metadata))
        {
            try
            {
                std::ifstream input(metadata);
                input >> sidecar;
                const auto clips = sidecar.value("clips", nlohmann::json::array());
                resourceWritable = sidecar.value("schemaVersion", 1) >= 1 && sidecar.value("schemaVersion", 1) <= 2 &&
                                   sidecar.value("keyframes", nlohmann::json::array()).empty() && clips.size() <= 1;
                if (!clips.empty())
                {
                    const auto &clip = clips.front();
                    resourceWritable =
                        resourceWritable && clip.value("start", 0.0) == 0 && clip.value("source", path) == path;
                    edit = {clip.value("sourceOffset", 0.0),
                            clip.value("sourceOffset", 0.0) + clip.value("duration", duration),
                            clip.value("gain", 1.0f), static_cast<float>(clip.value("fadeIn", 0.0)),
                            static_cast<float>(clip.value("fadeOut", 0.0))};
                }
                if (edit.begin < 0 || edit.end > duration || edit.begin >= edit.end || !std::isfinite(edit.gain) ||
                    edit.gain < 0 || edit.fadeIn < 0 || edit.fadeOut < 0 || !std::isfinite(edit.begin) ||
                    !std::isfinite(edit.end) || !std::isfinite(edit.fadeIn) || !std::isfinite(edit.fadeOut))
                {
                    resourceWritable = false;
                    edit = {0, duration, 1, 0, 0};
                }
            }
            catch (...)
            {
                resourceWritable = false;
                edit = {0, duration, 1, 0, 0};
            }
        }
        if (!resourceWritable)
            edit = {0, duration, 1, 0, 0};
        saved = edit;
        try
        {
            loop->setChecked(sidecar.value("preview", nlohmann::json::object()).value("loop", true));
        }
        catch (...)
        {
            loop->setChecked(true);
            resourceWritable = false;
        }
        wave->duration = duration;
        wave->position = edit.begin;
        wave->origin = 0;
        wave->visible = duration;
        wave->peaks.assign(512, 0);
        const auto *data = next->getData();
        const size_t bytes = next->getBitsPerSample() / 8;
        for (size_t f = 0; f < frames; ++f)
            for (size_t ch = 0; ch < next->getChannels(); ++ch)
            {
                float sample = 0;
                const auto *p = data + (f * next->getChannels() + ch) * bytes;
                if (bytes == 4)
                    std::memcpy(&sample, p, 4);
                else
                {
                    int16_t v;
                    std::memcpy(&v, p, 2);
                    sample = v / 32768.f;
                }
                if (std::isfinite(sample))
                    wave->peaks[std::min<size_t>(511, f * 512 / std::max<size_t>(1, frames))] = std::max(
                        wave->peaks[std::min<size_t>(511, f * 512 / std::max<size_t>(1, frames))], std::abs(sample));
            }
        resourceName->setText(std::filesystem::path(path).filename().wstring() + L" · " + workbench::number(duration) +
                              L" s");
        syncResource();
        return true;
    }
    bool equal(const Edit &a, const Edit &b) const
    {
        return a.begin == b.begin && a.end == b.end && a.gain == b.gain && a.fadeIn == b.fadeIn &&
               a.fadeOut == b.fadeOut;
    }
    void commitEdit(Edit before)
    {
        if (!equal(before, edit))
        {
            undo.push_back(before);
            if (undo.size() > 100)
                undo.erase(undo.begin());
            redo.clear();
            stopResource();
        }
        syncResource();
    }
    void applyResourceFields()
    {
        if (updating || !resource || !resourceWritable)
            return;
        try
        {
            Edit next{workbench::parse(fields[0]->getText()), workbench::parse(fields[1]->getText()),
                      static_cast<float>(workbench::parse(fields[2]->getText()) / 100),
                      static_cast<float>(workbench::parse(fields[3]->getText())),
                      static_cast<float>(workbench::parse(fields[4]->getText()))};
            if (next.begin < 0 || next.end > duration || next.begin >= next.end || next.gain < 0 || next.gain > 8 ||
                next.fadeIn < 0 || next.fadeOut < 0 || next.fadeIn > next.end - next.begin ||
                next.fadeOut > next.end - next.begin)
                throw std::runtime_error("invalid interval");
            const auto before = edit;
            edit = next;
            commitEdit(before);
        }
        catch (...)
        {
            resourceStatus->setText(L"数值无效：起点需小于终点，淡入淡出不得超过区间长度");
        }
    }
    void syncResource()
    {
        if (!wave)
            return;
        wave->begin = edit.begin;
        wave->end = edit.end;
        wave->position = std::clamp(wave->position, edit.begin, edit.end);
        wave->markDirty();
        updating = true;
        const double values[] = {edit.begin, edit.end, edit.gain * 100, edit.fadeIn, edit.fadeOut};
        for (int i = 0; i < 5; ++i)
            if (fields[i] && s._ui->getFocusedWidget() != fields[i])
            {
                fields[i]->setText(workbench::number(values[i], i == 2 ? 0 : 3));
                fields[i]->setReadOnly(!resourceWritable);
            }
        updating = false;
        if (resourceStatus)
            resourceStatus->setText(!resourceWritable    ? L"此资源含复杂时间轴；本工作台只读，请使用主编辑器时间轴"
                                    : equal(edit, saved) ? L"编辑信息已保存 · 原始音频保持不变"
                                                         : L"编辑信息未保存 · 原始音频保持不变");
    }
    void updateResourceGain()
    {
        if (resourceValid() && resourceVoice)
            s.liveEngine()->setVoiceVolume(resourceVoice, edit.gain * auditionGain);
    }
    void playResource()
    {
        if (!resource || !s.ensureClips())
            return;
        if (!resourceValid() || !resourceClip)
        {
            resourceClip = s._audio->registerClipFromResource(resource.get());
            resourceEngine = s.liveEngine();
            resourceGeneration = resourceEngine->lifecycleGeneration();
        }
        stopResource();
        resourceVoice = s.liveEngine()->play(resourceClip, bus(resourceBus->getSelectedIndex()), {},
                                             edit.gain * auditionGain, loop->isChecked());
        if (!resourceVoice)
            return;
        const auto rate = resource->getSampleRate();
        s.liveEngine()->setVoicePlaybackRange(resourceVoice, static_cast<uint64_t>(edit.begin * rate),
                                              static_cast<uint64_t>(edit.end * rate));
        s.liveEngine()->setVoiceEnvelope(resourceVoice, edit.fadeIn, edit.fadeOut);
        wave->position = edit.begin;
        resourceStatus->setText(L"资源试听中 · 暂停仅影响此资源");
    }
    void saveResource()
    {
        if (!resource || !resourceWritable)
            return;
        try
        {
            auto json = sidecar;
            json["schemaVersion"] = 2;
            json["source"] = path;
            json["durationSeconds"] = duration;
            json["keyframes"] = nlohmann::json::array();
            json["preview"] = {{"begin", 0.0}, {"end", edit.end - edit.begin}, {"loop", loop->isChecked()}};
            json["clips"] = nlohmann::json::array({{{"id", "clip.audio"},
                                                    {"track", "audio"},
                                                    {"source", path},
                                                    {"start", 0},
                                                    {"duration", edit.end - edit.begin},
                                                    {"sourceOffset", edit.begin},
                                                    {"gain", edit.gain},
                                                    {"fadeIn", edit.fadeIn},
                                                    {"fadeOut", edit.fadeOut}}});
            const auto bytes = json.dump(2) + "\n";
            if (!ayt::io::File::atomicWrite(path + ".timeline.json", bytes.data(), bytes.size()))
                throw std::runtime_error("write failed");
            sidecar = json;
            saved = edit;
            syncResource();
        }
        catch (...)
        {
            resourceStatus->setText(L"保存失败：无法写入编辑信息文件");
        }
    }
    void tick(float dt)
    {
        tickRecording();
        updating = true;
        for (auto &g : gains)
        {
            const float v = g.read() * g.scale;
            g.slider->setValue(v);
            if (s._ui->getFocusedWidget() != g.input)
                g.input->setText(workbench::number(v, g.scale == 1 ? 2 : 0));
        }
        updating = false;
        auto *e = s.liveEngine();
        if(tabs->getSelectedIndex()==3) {
            const auto state=recorder.status();AudioOutputLevels levels{};
            levels.left=levels.right=state.peak;levels.block=++recordingMeterBlock;levels.clipped=state.peak>=1;
            meter->title=state.recording?L"录音输入 L / R":L"录音回放 L / R";
            meter->update(levels,dt);
        } else {
            meter->title=L"输出 L / R";
            meter->update(e ? e->outputLevels() : AudioOutputLevels{}, dt);
        }
        if (resourceValid() && resourceVoice)
        {
            VoiceRuntime state;
            if (e->voiceState(resourceVoice, state))
            {
                wave->position = static_cast<double>(state.positionFrames) / resource->getSampleRate();
                wave->markDirty();
            }
            else
                resourceVoice = 0;
        }
        if (e)
            diagnostic->setText(L"诊断：活动声部 " + std::to_wstring(e->activeVoiceCount()) + L" · 流缓冲欠载 " +
                                std::to_wstring(e->streamUnderrunCount()) + L" · 当前键盘 C" +
                                std::to_wstring(s._octave) + L" · 失焦释放音符");
    }
};
