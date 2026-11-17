#include "PluginEditor.h"
using namespace vat;

namespace
{
using APVTS = juce::AudioProcessorValueTreeState;
float getP (APVTS& s, const char* id) { return s.getRawParameterValue (id)->load(); }
void setP (APVTS& s, const juce::String& id, float v)
{
    if (auto* p = s.getParameter (id)) { p->beginChangeGesture(); p->setValueNotifyingHost (p->convertTo0to1 (v)); p->endChangeGesture(); }
}
int currentMask (APVTS& s)
{
    int custom = 0; for (int i = 0; i < 12; ++i) if (getP (s, ("note" + juce::String (i)).toRawUTF8()) > 0.5f) custom |= 1 << i;
    return absoluteMask (juce::roundToInt (getP (s, "key")), juce::roundToInt (getP (s, "scale")), custom);
}

//==============================================================================
// One‑octave keyboard: lit keys are allowed targets; click to include/exclude (switches to Custom)
class ScaleKeyboard : public juce::Component
{
public:
    explicit ScaleKeyboard (VegaTuneProcessor& p) : proc (p) {}
    void paint (juce::Graphics& g) override
    {
        const int mask = currentMask (proc.apvts);
        const float target = proc.targetNote.load();
        const int tpc = target >= 0 ? (juce::roundToInt (target) % 12 + 12) % 12 : -1;
        auto b = getLocalBounds().toFloat();
        const float ww = b.getWidth() / 7.0f;
        static const int whites[7] { 0, 2, 4, 5, 7, 9, 11 };
        static const int blacks[5] { 1, 3, 6, 8, 10 }; static const float bx[5] { 1, 2, 4, 5, 6 };
        for (int i = 0; i < 7; ++i)
        {
            auto r = juce::Rectangle<float> (b.getX() + i * ww, b.getY(), ww, b.getHeight()).reduced (1.5f, 0);
            const int pc = whites[i]; const bool on = mask & (1 << pc);
            g.setColour (on ? juce::Colour (0xffe8e8e8) : juce::Colour (0xff4a4a4a)); g.fillRoundedRectangle (r, 3.0f);
            if (pc == tpc) { g.setColour (VegaColours::gold); g.fillRoundedRectangle (r.removeFromBottom (10).reduced (3, 2), 2.0f); }
            g.setColour (on ? juce::Colour (0xff222222) : VegaColours::textDim); g.setFont (VegaFonts::ui (10.0f, true));
            g.drawText (keyNames()[pc], r.withTrimmedBottom (12).toNearestInt(), juce::Justification::centredBottom);
            if (on) { g.setColour (VegaColours::accent); g.fillRect (r.getX() + 3, b.getY() + 2, r.getWidth() - 6, 3.0f); }
        }
        for (int i = 0; i < 5; ++i)
        {
            auto r = juce::Rectangle<float> (b.getX() + bx[i] * ww - ww * 0.32f, b.getY(), ww * 0.64f, b.getHeight() * 0.6f);
            const int pc = blacks[i]; const bool on = mask & (1 << pc);
            g.setColour (on ? juce::Colour (0xff101010) : juce::Colour (0xff3c3c3c)); g.fillRoundedRectangle (r, 2.0f);
            g.setColour (on ? VegaColours::accent : juce::Colour (0xff4a4a4a)); g.drawRoundedRectangle (r, 2.0f, 1.2f);
            if (on) { g.setColour (VegaColours::accent); g.fillRect (r.getX() + 3, r.getY() + 2, r.getWidth() - 6, 3.0f); }
            if (pc == tpc) { g.setColour (VegaColours::gold); g.fillRect (r.reduced (4).removeFromBottom (6)); }
        }
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        auto b = getLocalBounds().toFloat(); const float ww = b.getWidth() / 7.0f;
        static const int whites[7] { 0, 2, 4, 5, 7, 9, 11 };
        static const int blacks[5] { 1, 3, 6, 8, 10 }; static const float bx[5] { 1, 2, 4, 5, 6 };
        int pc = -1;
        if (e.position.y < b.getHeight() * 0.6f)
            for (int i = 0; i < 5; ++i) if (std::abs (e.position.x - (b.getX() + bx[i] * ww)) < ww * 0.32f) pc = blacks[i];
        if (pc < 0) pc = whites[juce::jlimit (0, 6, (int) ((e.position.x - b.getX()) / ww))];
        int mask = currentMask (proc.apvts) ^ (1 << pc);
        for (int i = 0; i < 12; ++i) setP (proc.apvts, "note" + juce::String (i), (mask >> i) & 1 ? 1.0f : 0.0f);
        setP (proc.apvts, "scale", (float) kCustomScale);
        repaint();
    }
private:
    VegaTuneProcessor& proc;
};

//==============================================================================
// Tuning meter: needle = input deviation from target note (cents); inner OLED shows notes
class TuningMeter : public juce::Component
{
public:
    explicit TuningMeter (VegaTuneProcessor& p) : proc (p) {}
    void refresh()
    {
        const float in = proc.inMidi.load(), t = proc.targetNote.load(), out = proc.outMidi.load();
        if (in >= 0 && t >= 0) { devIn += ((in - t) * 100.0f - devIn) * 0.35f; devOut += ((out - t) * 100.0f - devOut) * 0.35f; voiced = true; }
        else voiced = false;
        repaint();
    }
    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        vegaui::drawScreen (g, b);
        const auto c = juce::Point<float> (b.getCentreX(), b.getBottom() - 40);
        const float R = std::min (b.getWidth() * 0.42f, b.getHeight() - 70);
        const float a0 = -juce::MathConstants<float>::pi * 0.38f, a1 = -a0;
        auto ang = [&] (float cents) { return juce::jmap (juce::jlimit (-50.0f, 50.0f, cents), -50.0f, 50.0f, a0, a1); };
        // scale ticks
        for (int ct = -50; ct <= 50; ct += 5)
        {
            const float a = ang ((float) ct); const bool major = ct % 25 == 0;
            g.setColour (ct == 0 ? VegaColours::accent : VegaColours::textDim.withAlpha (major ? 0.9f : 0.4f));
            const float r1 = R - (major ? 16.0f : 9.0f);
            g.drawLine (c.x + r1 * std::sin (a), c.y - r1 * std::cos (a), c.x + R * std::sin (a), c.y - R * std::cos (a), ct == 0 ? 2.5f : 1.2f);
            if (major) { g.setFont (VegaFonts::ui (10.0f)); g.drawText ((ct > 0 ? "+" : "") + juce::String (ct), juce::Rectangle<int> (40, 14).withCentre ({ (int) (c.x + (R + 14) * std::sin (a)), (int) (c.y - (R + 14) * std::cos (a)) }), juce::Justification::centred); }
        }
        if (voiced)
        {
            // input needle (grey) and corrected needle (cyan)
            for (int k = 0; k < 2; ++k)
            {
                const float a = ang (k == 0 ? devIn : devOut);
                g.setColour (k == 0 ? juce::Colours::white.withAlpha (0.35f) : VegaColours::accent);
                g.drawLine (c.x, c.y, c.x + (R - 4) * std::sin (a), c.y - (R - 4) * std::cos (a), k == 0 ? 2.0f : 3.0f);
            }
        }
        g.setColour (juce::Colour (0xff2a2a2a)); g.fillEllipse (c.x - 7, c.y - 7, 14, 14);
        // read‑out
        g.setFont (VegaFonts::mono (30.0f)); g.setColour (juce::Colours::white);
        const float t = proc.targetNote.load();
        g.drawText (voiced ? midiName (t) : juce::String ("--"), juce::Rectangle<float> (b.getX(), c.y - R * 0.62f, b.getWidth(), 34).toNearestInt(), juce::Justification::centred);
        g.setFont (VegaFonts::mono (11.5f)); g.setColour (VegaColours::textDim);
        const float in = proc.inMidi.load();
        g.drawText (voiced ? "IN " + midiName (in) + juce::String::formatted ("  %+.0f ct", devIn) : juce::String ("NO PITCH"),
                    juce::Rectangle<float> (b.getX(), c.y - R * 0.62f + 36, b.getWidth(), 16).toNearestInt(), juce::Justification::centred);
        g.setFont (VegaFonts::ui (10.0f, true)); g.setColour (VegaColours::textDim);
        g.drawText ("TUNING", b.withHeight (18).translated (8, 4).toNearestInt(), juce::Justification::left);
        g.setColour (juce::Colours::white.withAlpha (0.4f)); g.drawText ("INPUT", juce::Rectangle<int> ((int) b.getX() + 10, (int) b.getBottom() - 20, 60, 14), juce::Justification::left);
        g.setColour (VegaColours::accent); g.drawText ("CORRECTED", juce::Rectangle<int> ((int) b.getRight() - 90, (int) b.getBottom() - 20, 80, 14), juce::Justification::right);
    }
private:
    VegaTuneProcessor& proc; float devIn = 0, devOut = 0; bool voiced = false;
};

//==============================================================================
// Scrolling pitch history: input (grey) vs corrected output (cyan) on a note grid
class PitchHistory : public juce::Component
{
public:
    explicit PitchHistory (VegaTuneProcessor& p) : proc (p) { pts.resize (1600, { -1, -1 }); }
    void refresh()
    {
        int s1, n1, s2, n2;
        proc.historyFifo.prepareToRead (proc.historyFifo.getNumReady(), s1, n1, s2, n2);
        auto push = [&] (const VegaTuneProcessor::HistoryPoint& h) { pts[(size_t) pos] = h; pos = (pos + 1) % (int) pts.size(); };
        for (int i = 0; i < n1; ++i) push (proc.historyBuf[(size_t) (s1 + i)]);
        for (int i = 0; i < n2; ++i) push (proc.historyBuf[(size_t) (s2 + i)]);
        proc.historyFifo.finishedRead (n1 + n2);
        // follow the singer's register
        float lo = 1e9f, hi = -1e9f;
        for (auto& h : pts) if (h.in >= 0) { lo = std::min (lo, h.in); hi = std::max (hi, h.in); }
        if (lo < 1e8f) { const float mid = 0.5f * (lo + hi), span = std::max (12.0f, hi - lo + 4); vLo += (mid - span / 2 - vLo) * 0.08f; vHi += (mid + span / 2 - vHi) * 0.08f; }
        repaint();
    }
    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat(); vegaui::drawScreen (g, b);
        auto r = b.reduced (36.0f, 10.0f).withTrimmedLeft (-6);
        const int mask = currentMask (proc.apvts);
        auto yFor = [&] (float m) { return juce::jmap (m, vLo, vHi, r.getBottom(), r.getY()); };
        g.setFont (VegaFonts::ui (9.5f));
        for (int nn = (int) std::floor (vLo); nn <= (int) std::ceil (vHi); ++nn)
        {
            const float y = yFor ((float) nn); if (y < r.getY() - 1 || y > r.getBottom() + 1) continue;
            const bool on = mask & (1 << ((nn % 12 + 12) % 12));
            g.setColour (juce::Colours::white.withAlpha (on ? 0.09f : 0.025f));
            g.drawHorizontalLine ((int) y, r.getX(), r.getRight());
            if (on) { g.setColour (VegaColours::textDim); g.drawText (midiName ((float) nn), 2, (int) y - 6, 30, 12, juce::Justification::centredRight); }
        }
        const int N = (int) pts.size();
        for (int pass = 0; pass < 2; ++pass)
        {
            juce::Path path; bool pen = false;
            for (int i = 0; i < N; ++i)
            {
                const auto& h = pts[(size_t) ((pos + i) % N)];
                const float v = pass == 0 ? h.in : h.out;
                const float x = r.getX() + r.getWidth() * i / (N - 1);
                if (v < 0) { pen = false; continue; }
                if (! pen) { path.startNewSubPath (x, yFor (v)); pen = true; } else path.lineTo (x, yFor (v));
            }
            g.setColour (pass == 0 ? juce::Colours::white.withAlpha (0.35f) : VegaColours::accent);
            g.strokePath (path, juce::PathStrokeType (pass == 0 ? 1.4f : 2.0f));
        }
        g.setFont (VegaFonts::ui (10.0f, true)); g.setColour (VegaColours::textDim);
        g.drawText ("PITCH HISTORY", 40, 4, 200, 14, juce::Justification::left);
    }
private:
    VegaTuneProcessor& proc; std::vector<VegaTuneProcessor::HistoryPoint> pts; int pos = 0; float vLo = 48, vHi = 72;
};

//==============================================================================
// Graph mode: tracked pitch on the host timeline, split into notes; drag notes to retune
class GraphEditor : public juce::Component
{
public:
    explicit GraphEditor (VegaTuneProcessor& p) : proc (p) {}
    bool follow = true;
    double viewStart = 0, viewLen = 10;

    struct Note { int start, end; float mean; bool edited; float shift; };

    void rebuild()
    {
        notes.clear();
        const int a = std::max (0, (int) (viewStart * 100) - 200), b = std::min (VegaTuneProcessor::kMaxTicks, (int) ((viewStart + viewLen) * 100) + 200);
        Note cur { -1, -1, 0, false, 0 }; double sum = 0; int count = 0, gap = 0;
        auto close = [&]
        {
            if (count >= 6)
            {
                cur.mean = (float) (sum / count);
                const int mid = (cur.start + cur.end) / 2;
                cur.edited = proc.hasEdit[(size_t) mid].load(); cur.shift = proc.editShift[(size_t) mid].load();
                notes.push_back (cur);
            }
            count = 0; sum = 0;
        };
        for (int i = a; i < b; ++i)
        {
            const float v = proc.trackMidi[(size_t) i].load();
            if (v < 0) { if (count > 0 && ++gap > 3) close(); continue; }
            if (count > 0 && std::abs (v - (float) (sum / count)) > 0.6f) close();
            if (count == 0) { cur.start = i; gap = 0; }
            sum += v; ++count; cur.end = i; gap = 0;
        }
        close();
    }

    void refresh()
    {
        const double ph = proc.playheadSeconds.load();
        if (follow && proc.hostPlaying.load() && ph >= 0 && (ph < viewStart || ph > viewStart + viewLen * 0.85)) viewStart = std::max (0.0, ph - viewLen * 0.15);
        rebuild(); repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat(); vegaui::drawScreen (g, b);
        auto r = plot();
        // vertical range from visible data
        float lo = 1e9f, hi = -1e9f;
        for (auto& n : notes) { lo = std::min (lo, n.mean + (n.edited ? n.shift : 0) - 1); hi = std::max (hi, n.mean + (n.edited ? n.shift : 0) + 1); lo = std::min (lo, n.mean - 1); hi = std::max (hi, n.mean + 1); }
        if (lo > 1e8f) { lo = 48; hi = 72; }
        const float span = std::max (14.0f, hi - lo + 4), mid = 0.5f * (lo + hi);
        yLo = mid - span / 2; yHi = mid + span / 2;
        const int mask = currentMask (proc.apvts);
        g.setFont (VegaFonts::ui (9.5f));
        for (int nn = (int) std::floor (yLo); nn <= (int) std::ceil (yHi); ++nn)
        {
            const float y0 = yFor (nn + 0.5f), y1 = yFor (nn - 0.5f);
            const bool on = mask & (1 << ((nn % 12 + 12) % 12)); const bool black = juce::String ("010100101010").substring ((nn % 12 + 12) % 12, (nn % 12 + 12) % 12 + 1) == "1";
            g.setColour (black ? juce::Colour (0xff111111) : juce::Colour (0xff181818)); g.fillRect (r.getX(), y0, r.getWidth(), y1 - y0);
            g.setColour (juce::Colours::white.withAlpha (on ? 0.07f : 0.02f)); g.drawHorizontalLine ((int) y1, r.getX(), r.getRight());
            g.setColour (on ? VegaColours::text : VegaColours::textDim.withAlpha (0.5f));
            g.drawText (midiName ((float) nn), (int) b.getX() + 2, (int) ((y0 + y1) / 2) - 6, 34, 12, juce::Justification::centredRight);
        }
        // time grid
        for (int s = (int) viewStart; s <= (int) (viewStart + viewLen) + 1; ++s)
        {
            const float x = xFor (s); if (x < r.getX() || x > r.getRight()) continue;
            g.setColour (juce::Colours::white.withAlpha (0.06f)); g.drawVerticalLine ((int) x, r.getY(), r.getBottom());
            g.setColour (VegaColours::textDim); g.drawText (juce::String (s / 60) + ":" + juce::String (s % 60).paddedLeft ('0', 2), (int) x - 20, (int) r.getBottom() + 2, 40, 12, juce::Justification::centred);
        }
        g.saveState(); g.reduceClipRegion (r.toNearestInt());
        // notes
        for (int i = 0; i < (int) notes.size(); ++i)
        {
            const auto& n = notes[(size_t) i];
            const float pitch = n.mean + (n.edited ? n.shift : 0.0f);
            auto blob = juce::Rectangle<float>::leftTopRightBottom (xFor (n.start / 100.0), yFor (pitch + 0.42f), xFor ((n.end + 1) / 100.0), yFor (pitch - 0.42f));
            g.setColour ((n.edited ? VegaColours::accent : juce::Colour (0xff6a6a6a)).withAlpha (i == hover || i == drag ? 0.55f : 0.32f));
            g.fillRoundedRectangle (blob, 4.0f);
            g.setColour (n.edited ? VegaColours::accent : juce::Colour (0xff8a8a8a)); g.drawRoundedRectangle (blob, 4.0f, 1.2f);
        }
        // raw curve (grey) and corrected curve (cyan, where edited)
        for (int pass = 0; pass < 2; ++pass)
        {
            juce::Path path; bool pen = false;
            const int a = std::max (0, (int) (viewStart * 100)), e = std::min (VegaTuneProcessor::kMaxTicks, (int) ((viewStart + viewLen) * 100) + 1);
            for (int i = a; i < e; ++i)
            {
                float v = proc.trackMidi[(size_t) i].load();
                if (v < 0 || (pass == 1 && ! proc.hasEdit[(size_t) i].load())) { pen = false; continue; }
                if (pass == 1) v += proc.editShift[(size_t) i].load();
                const float x = xFor (i / 100.0), y = yFor (v);
                if (! pen) { path.startNewSubPath (x, y); pen = true; } else path.lineTo (x, y);
            }
            g.setColour (pass == 0 ? juce::Colours::white.withAlpha (0.5f) : VegaColours::accent);
            g.strokePath (path, juce::PathStrokeType (pass == 0 ? 1.2f : 1.8f));
        }
        const double ph = proc.playheadSeconds.load();
        if (ph >= viewStart && ph <= viewStart + viewLen) { g.setColour (VegaColours::gold); g.drawVerticalLine ((int) xFor (ph), r.getY(), r.getBottom()); }
        g.restoreState();
        if (notes.empty())
        {
            g.setColour (VegaColours::textDim); g.setFont (VegaFonts::ui (13.0f));
            g.drawFittedText ("Press TRACK, then play your vocal in the DAW.\nThe pitch is drawn here against the song timeline and split into notes.\n"
                              "Drag a note up/down to retune it (hold Shift for fine cents), double-click to reset it.\n"
                              "Drag empty space to scroll, Ctrl + wheel to zoom.", r.toNearestInt(), juce::Justification::centred, 6);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override { const int h = hit (e.position); if (h != hover) { hover = h; repaint(); } }
    void mouseDown (const juce::MouseEvent& e) override
    {
        drag = hit (e.position); dragY = e.position.y; scrollStart = viewStart;
        if (drag >= 0) { const auto& n = notes[(size_t) drag]; startShift = n.edited ? n.shift : 0.0f; }
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (drag < 0)
        {
            viewStart = std::max (0.0, scrollStart - (e.position.x - e.mouseDownPosition.x) / plot().getWidth() * viewLen);
            follow = false; repaint(); return;
        }
        auto& n = notes[(size_t) drag];
        const float dy = (dragY - e.position.y) / (plot().getHeight() / (yHi - yLo));
        float newPitch = n.mean + startShift + dy;
        if (! e.mods.isShiftDown()) newPitch = (float) juce::roundToInt (newPitch);
        applyShift (n, newPitch - n.mean);
        n.edited = true; n.shift = newPitch - n.mean;
        repaint();
    }
    void mouseUp (const juce::MouseEvent&) override { drag = -1; }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int h = hit (e.position);
        if (h >= 0) { auto& n = notes[(size_t) h]; for (int i = n.start; i <= n.end; ++i) proc.hasEdit[(size_t) i].store (false); refresh(); }
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCtrlDown() || e.mods.isCommandDown())
        {
            const double anchor = viewStart + (e.position.x - plot().getX()) / plot().getWidth() * viewLen;
            viewLen = juce::jlimit (1.0, 120.0, viewLen * (w.deltaY > 0 ? 0.8 : 1.25));
            viewStart = std::max (0.0, anchor - (e.position.x - plot().getX()) / plot().getWidth() * viewLen);
        }
        else viewStart = std::max (0.0, viewStart - (w.deltaY + w.deltaX) * viewLen * 0.3);
        follow = false; refresh();
    }

    /** Snap every tracked note in the whole song to the nearest allowed scale note. */
    void snapAll()
    {
        const double vs = viewStart, vl = viewLen;
        viewStart = 0; viewLen = VegaTuneProcessor::kMaxTicks / 100.0; rebuild();
        const int mask = currentMask (proc.apvts);
        for (auto& n : notes)
        {
            int best = juce::roundToInt (n.mean); float bd = 99;
            for (int k = -6; k <= 6; ++k) { const int c = juce::roundToInt (n.mean) + k; if ((mask & (1 << ((c % 12 + 12) % 12))) && std::abs (c - n.mean) < bd) { bd = std::abs (c - n.mean); best = c; } }
            applyShift (n, (float) best - n.mean);
        }
        viewStart = vs; viewLen = vl; refresh();
    }

private:
    juce::Rectangle<float> plot() const { return getLocalBounds().toFloat().reduced (8.0f, 8.0f).withTrimmedLeft (32).withTrimmedBottom (14); }
    float xFor (double s) const { auto r = plot(); return r.getX() + (float) ((s - viewStart) / viewLen) * r.getWidth(); }
    float yFor (float m) const { auto r = plot(); return juce::jmap (m, yLo, yHi, r.getBottom(), r.getY()); }
    int hit (juce::Point<float> p) const
    {
        for (int i = 0; i < (int) notes.size(); ++i)
        {
            const auto& n = notes[(size_t) i]; const float pitch = n.mean + (n.edited ? n.shift : 0.0f);
            if (juce::Rectangle<float>::leftTopRightBottom (xFor (n.start / 100.0), yFor (pitch + 0.5f), xFor ((n.end + 1) / 100.0), yFor (pitch - 0.5f)).contains (p)) return i;
        }
        return -1;
    }
    void applyShift (const Note& n, float shift)
    {
        for (int i = n.start; i <= n.end; ++i) { proc.editShift[(size_t) i].store (shift); proc.hasEdit[(size_t) i].store (true); }
    }
    VegaTuneProcessor& proc; std::vector<Note> notes; int hover = -1, drag = -1; float dragY = 0, startShift = 0, yLo = 48, yHi = 72; double scrollStart = 0;
};
} // namespace

//==============================================================================
class VegaTuneEditor::Main : public juce::Component
{
public:
    Main (VegaTuneProcessor& p) : proc (p), a (p.apvts),
        key (a, "key", "Key"), scale (a, "scale", "Scale"), range (a, "range", "Voice"),
        latency (a, "latency", "Mode", "Studio|Live"), midiMode (a, "midiMode", "MIDI Notes"), style (a, "style", "Style", "Natural|Hard"),
        retune (a, "retune", "Retune Speed", true), humanize (a, "humanize", "Humanize"), flex (a, "flex", "Flex-Tune"),
        vibKeep (a, "vibKeep", "Natural Vibrato"), amount (a, "amount", "Amount"),
        vibDepth (a, "vibDepth", "Depth"), vibRate (a, "vibRate", "Rate"), vibDelay (a, "vibDelay", "Onset"),
        transpose (a, "transpose", "Transpose"), formant (a, "formant", "Formant"), mix (a, "mix", "Mix"), output (a, "output", "Output"),
        graphAuto (a, "graphAuto", "Unedited parts", "As sung|Auto-tune"),
        keyboard (p), meter (p), history (p), graph (p)
    {
        for (auto* c : autoComps()) addAndMakeVisible (c);
        addChildComponent (graph); addChildComponent (graphAuto);
        detect.setButtonText ("DETECT KEY"); detect.onClick = [this] { detectKey(); }; addAndMakeVisible (detect);
        listen.setButtonText ("RESET"); listen.setTooltip ("Clear what Auto-Key has heard"); listen.onClick = [this] { proc.resetHistogram(); keyInfo = "Listening..."; repaint(); };
        addAndMakeVisible (listen);
        const char* tn[] { "AUTO", "GRAPH" };
        for (int i = 0; i < 2; ++i)
        {
            tabs[(size_t) i].setButtonText (tn[i]); tabs[(size_t) i].setClickingTogglesState (true); tabs[(size_t) i].setRadioGroupId (5);
            tabs[(size_t) i].onClick = [this, i] { showMode (i); }; addAndMakeVisible (tabs[(size_t) i]);
        }
        bypass.setClickingTogglesState (true);
        bypassAtt = std::make_unique<APVTS::ButtonAttachment> (a, "bypass", bypass); addAndMakeVisible (bypass);
        track.setButtonText ("TRACK"); track.setClickingTogglesState (true);
        track.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffe0505a));
        track.setToggleState (proc.tracking.load(), juce::dontSendNotification);
        track.onClick = [this] { proc.tracking.store (track.getToggleState()); };
        clearTrack.setButtonText ("CLEAR PITCH"); clearTrack.onClick = [this] { proc.clearTrack(); proc.clearEdits(); graph.refresh(); };
        clearEdits.setButtonText ("RESET EDITS"); clearEdits.onClick = [this] { proc.clearEdits(); graph.refresh(); };
        snap.setButtonText ("SNAP ALL TO SCALE"); snap.onClick = [this] { graph.snapAll(); };
        followBtn.setButtonText ("FOLLOW"); followBtn.setClickingTogglesState (true); followBtn.setToggleState (true, juce::dontSendNotification);
        followBtn.onClick = [this] { graph.follow = followBtn.getToggleState(); };
        for (auto* b : { &track, &clearTrack, &clearEdits, &snap, &followBtn }) addChildComponent (b);
        showMode (proc.uiMode.load());
    }

    std::vector<juce::Component*> autoComps()
    {
        return { &key, &scale, &range, &latency, &midiMode, &style, &retune, &humanize, &flex, &vibKeep, &amount,
                 &vibDepth, &vibRate, &vibDelay, &transpose, &formant, &mix, &output, &keyboard, &meter, &history };
    }

    void showMode (int m)
    {
        proc.uiMode.store (m);
        for (int i = 0; i < 2; ++i) tabs[(size_t) i].setToggleState (i == m, juce::dontSendNotification);
        for (auto* c : autoComps()) c->setVisible (m == 0);
        detect.setVisible (m == 0); listen.setVisible (m == 0);
        graph.setVisible (m == 1); graphAuto.setVisible (m == 1);
        for (auto* b : { &track, &clearTrack, &clearEdits, &snap, &followBtn }) b->setVisible (m == 1);
        if (m == 1) graph.refresh();
        repaint();
    }

    void detectKey()
    {
        float strength = 0; auto [k, s] = proc.detectKey (strength);
        if (k < 0) { keyInfo = "Need a few seconds of singing first"; repaint(); return; }
        setP (a, "key", (float) k); setP (a, "scale", (float) s);
        keyInfo = "Detected: " + keyNames()[k] + (s == 1 ? " major" : " minor") + juce::String::formatted ("  (%.0f%%)", juce::jlimit (0.0f, 1.0f, strength) * 100);
        repaint();
    }

    void refresh()
    {
        if (proc.uiMode.load() == 0) { meter.refresh(); history.refresh(); keyboard.repaint(); }
        else graph.refresh();
        const int lat = proc.getLatencySamples();
        if (lat != lastLat) { lastLat = lat; repaint (0, 0, W, 34); }
        track.setToggleState (proc.tracking.load(), juce::dontSendNotification);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (VegaColours::background);
        vegaui::drawTopBar (g, W, "AUTOTUNE  VP-8");
        vegaui::drawBody (g, { 0, 35, W, H - 65 });
        g.setFont (VegaFonts::ui (10.5f)); g.setColour (VegaColours::textDim);
        const double ms = lastLat * 1000.0 / std::max (1.0, proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0);
        g.drawText ("LATENCY " + juce::String (ms, 1) + " ms", W - 300, 0, 200, 34, juce::Justification::centredRight);
        if (proc.uiMode.load() == 0)
        {
            vegaui::drawSection (g, { 0, 76, 400, 300 }, "KEY  &  SCALE", false);
            vegaui::drawSection (g, { 400, 76, 460, 300 }, "PITCH");
            vegaui::drawSection (g, { 860, 76, W - 860, 300 }, "CORRECTION");
            vegaui::drawSection (g, { 860, 384, 210, 300 }, "ADD VIBRATO");
            vegaui::drawSection (g, { 1070, 384, W - 1070, 300 }, "VOICE  /  OUTPUT");
            g.setColour (VegaColours::line); g.drawHorizontalLine (380, 0, (float) W);
            g.setFont (VegaFonts::ui (10.5f)); g.setColour (keyInfo.startsWith ("Detected") ? VegaColours::accent : VegaColours::textDim);
            g.drawText (keyInfo, 16, 214, 370, 14, juce::Justification::left);
        }
        vegaui::drawBottomBar (g, { 0, H - 30, W, 30 }, proc.uiMode.load() == 0 ? "AUTOMATIC MODE  -  REAL-TIME PITCH CORRECTION" : "GRAPH MODE  -  TIMELINE-LOCKED NOTE EDITING");
    }

    void resized() override
    {
        tabs[0].setBounds (12, 44, 90, 24); tabs[1].setBounds (106, 44, 90, 24);
        bypass.setBounds (W - 76, 7, 66, 20);
        // KEY & SCALE
        key.setBounds (12, 92, 90, 46); scale.setBounds (106, 92, 150, 46); detect.setBounds (264, 108, 86, 24); listen.setBounds (352, 108, 40, 24);
        range.setBounds (12, 148, 150, 46); latency.setBounds (176, 156, 110, 50); midiMode.setBounds (300, 140, 90, 74);
        keyboard.setBounds (16, 234, 368, 132);
        // PITCH
        meter.setBounds (410, 94, 440, 280);
        history.setBounds (10, 390, 840, 290);
        // CORRECTION
        retune.setBounds (870, 90, 150, 180);
        humanize.setBounds (1030, 90, 88, 110); flex.setBounds (1118, 90, 88, 110); vibKeep.setBounds (1206, 90, 92, 110);
        amount.setBounds (1030, 236, 88, 110); style.setBounds (1130, 270, 120, 60);
        // ADD VIBRATO
        vibDepth.setBounds (870, 400, 95, 120); vibRate.setBounds (968, 400, 95, 120); vibDelay.setBounds (918, 540, 95, 120);
        // VOICE / OUTPUT
        transpose.setBounds (1080, 400, 105, 120); formant.setBounds (1188, 400, 105, 120);
        mix.setBounds (1080, 540, 105, 120); output.setBounds (1188, 540, 105, 120);
        // GRAPH
        track.setBounds (220, 44, 80, 24); clearTrack.setBounds (306, 44, 100, 24); clearEdits.setBounds (412, 44, 100, 24);
        snap.setBounds (518, 44, 150, 24); followBtn.setBounds (674, 44, 80, 24);
        graphAuto.setBounds (770, 36, 140, 52);
        graph.setBounds (10, 92, W - 20, H - 130);
    }

private:
    VegaTuneProcessor& proc; APVTS& a;
    vegaui::Choice key, scale, range; vegaui::Toggle latency, midiMode, style;
    vegaui::Knob retune, humanize, flex, vibKeep, amount, vibDepth, vibRate, vibDelay, transpose, formant, mix, output;
    vegaui::Toggle graphAuto;
    ScaleKeyboard keyboard; TuningMeter meter; PitchHistory history; GraphEditor graph;
    juce::TextButton detect, listen, bypass { "BYPASS" }, track, clearTrack, clearEdits, snap, followBtn;
    std::array<juce::TextButton, 2> tabs;
    std::unique_ptr<APVTS::ButtonAttachment> bypassAtt;
    juce::String keyInfo { "Auto-Key listens while you sing" }; int lastLat = 0;
};

//==============================================================================
VegaTuneEditor::VegaTuneEditor (VegaTuneProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&lnf);
    main = std::make_unique<Main> (p);
    addAndMakeVisible (*main);
    constrainer.setFixedAspectRatio ((double) W / H);
    constrainer.setSizeLimits (W / 2, H / 2, W * 2, H * 2);
    setConstrainer (&constrainer);
    setResizable (true, true);
    const float s = juce::jlimit (0.5f, 2.0f, proc.uiScale.load());
    setSize (juce::roundToInt (W * s), juce::roundToInt (H * s));
    startTimerHz (30);
}
VegaTuneEditor::~VegaTuneEditor() { stopTimer(); setConstrainer (nullptr); main.reset(); setLookAndFeel (nullptr); }
void VegaTuneEditor::timerCallback() { main->refresh(); }
void VegaTuneEditor::resized()
{
    main->setBounds (0, 0, W, H);
    const float s = (float) getWidth() / (float) W;
    main->setTransform (juce::AffineTransform::scale (s));
    proc.uiScale.store (s);
}

juce::AudioProcessorEditor* VegaTuneProcessor::createEditor() { return new VegaTuneEditor (*this); }
