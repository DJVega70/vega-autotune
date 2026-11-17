#include "PluginProcessor.h"
#include "PluginEditor.h"
using namespace vat;

VegaTuneProcessor::VegaTuneProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "VegaAutotune", createLayout()),
      trackMidi (kMaxTicks), editShift (kMaxTicks), hasEdit (kMaxTicks)
{
    for (auto& h : pcHist) h.store (0.0f);
    for (int i = 0; i < 12; ++i) noteParams[(size_t) i] = apvts.getRawParameterValue ("note" + juce::String (i));
    clearTrack(); clearEdits();
    startTimerHz (10);
}
VegaTuneProcessor::~VegaTuneProcessor() { stopTimer(); }

bool VegaTuneProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto o = l.getMainOutputChannelSet();
    return (o == juce::AudioChannelSet::mono() || o == juce::AudioChannelSet::stereo()) && l.getMainInputChannelSet() == o;
}

void VegaTuneProcessor::clearTrack() { for (auto& t : trackMidi) t.store (-1.0f); }
void VegaTuneProcessor::clearEdits() { for (int i = 0; i < kMaxTicks; ++i) { editShift[(size_t) i].store (0.0f); hasEdit[(size_t) i].store (false); } }

void VegaTuneProcessor::configure()
{
    curRange = juce::roundToInt (p ("range"));
    curLatencyMode = juce::roundToInt (p ("latency"));
    const bool live = curLatencyMode == 1;
    const float minHz = rangeMinHz[curRange], maxHz = rangeMaxHz[curRange];
    hop = std::max (16, juce::roundToInt ((live ? 64.0 : 128.0) * fs / 48000.0));
    yin.prepare (fs, minHz, maxHz);
    window.assign ((size_t) yin.samplesNeeded(), 0.0f);
    int sz = 1; while (sz < yin.samplesNeeded() * 2) sz <<= 1;
    mono.assign ((size_t) sz, 0.0f); monoPos = 0; hopCounter = 0;
    psola.prepare ((int) std::ceil (fs / minHz), hop, channels, live ? 0.85f : 0.7f);
    latencyNow.store (psola.getLatency());
    for (auto& r : dryRing) r.assign (1 << 16, 0.0f);
    dryPos = 0; center = -1; corr = 0; curNote = -1;
}

void VegaTuneProcessor::prepareToPlay (double sr, int)
{
    fs = sr;
    channels = std::max (1, getTotalNumOutputChannels());
    configure();
    setLatencySamples (latencyNow.load());
    outGain.reset (sr, 0.03); mixS.reset (sr, 0.03); bypassS.reset (sr, 0.02);
    outGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (p ("output")));
    mixS.setCurrentAndTargetValue (p ("mix") * 0.01f);
    bypassS.setCurrentAndTargetValue (p ("bypass") > 0.5f ? 1.0f : 0.0f);
}

void VegaTuneProcessor::timerCallback()
{
    if (latencyNow.load() != getLatencySamples()) setLatencySamples (latencyNow.load());
}

std::pair<int, int> VegaTuneProcessor::detectKey (float& strength) const
{
    static const float maj[12] { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
    static const float mnr[12] { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };
    float h[12]; float sum = 0;
    for (int i = 0; i < 12; ++i) { h[i] = pcHist[(size_t) i].load(); sum += h[i]; }
    if (sum < 1.0f) { strength = 0; return { -1, -1 }; }       // need about a second of singing
    auto corr = [&] (const float* prof, int key)
    {
        float mh = sum / 12, mp = 0; for (int i = 0; i < 12; ++i) mp += prof[i]; mp /= 12;
        float num = 0, dh = 0, dp = 0;
        for (int i = 0; i < 12; ++i) { const float a = h[(i + key) % 12] - mh, b = prof[i] - mp; num += a * b; dh += a * a; dp += b * b; }
        return num / std::sqrt (std::max (1.0e-9f, dh * dp));
    };
    float best = -2; int bk = 0, bs = 1;
    for (int k = 0; k < 12; ++k)
    {
        const float a = corr (maj, k), b = corr (mnr, k);
        if (a > best) { best = a; bk = k; bs = 1; }
        if (b > best) { best = b; bk = k; bs = 2; }
    }
    strength = best;
    return { bk, bs };
}

void VegaTuneProcessor::analyseHop (double hostSeconds, bool playing)
{
    // gather the most recent analysis window
    const int W = (int) window.size(), mask = (int) mono.size() - 1;
    for (int i = 0; i < W; ++i) window[(size_t) i] = mono[(size_t) ((monoPos - W + i) & mask)];
    float conf = 0;
    const float period = yin.detect (window.data(), conf);
    const bool voiced = period > 1.0f && conf > 0.55f;
    const float dt = (float) hop / (float) fs;
    const float m = voiced ? 69.0f + 12.0f * std::log2 ((float) fs / period / 440.0f) : -1.0f;

    // ---- graph tracking (timeline locked)
    const int tick = (int) std::floor (hostSeconds * kTicksPerSecond);
    const bool tickValid = playing && hostSeconds >= 0 && tick < kMaxTicks;
    if (tickValid && tracking.load()) trackMidi[(size_t) tick].store (m);

    const float formantFactor = std::pow (2.0f, p ("formant") / 12.0f);
    const float transpose = (float) juce::roundToInt (p ("transpose"));
    if (! voiced)
    {
        center = -1; curNote = -1; heldSeconds = 0; noteSeconds = 0;
        corr *= 0.9f;
        psola.setTarget (0.0f, 1.0f, formantFactor);
        inMidi.store (-1); outMidi.store (-1); targetNote.store (-1); confidence.store (conf);
        int s1, n1, s2, n2; historyFifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 > 0) historyBuf[(size_t) s1] = { -1, -1 };
        historyFifo.finishedWrite (n1);
        return;
    }

    const bool hard = juce::roundToInt (p ("style")) == 1;
    float retuneMs = hard ? 0.0f : p ("retune");
    const float humanize = hard ? 0.0f : p ("humanize") * 0.01f;
    const float flex = hard ? 0.0f : p ("flex") * 0.01f;
    const float vibKeep = hard ? 0.0f : p ("vibKeep") * 0.01f;
    const float amount = p ("amount") * 0.01f;

    // centre pitch (slow) + vibrato (fast) separation
    if (center < 0) center = m;
    else center += (m - center) * (1.0f - std::exp (-dt * juce::MathConstants<float>::twoPi * 3.0f));
    const float vib = m - center;

    // ---- choose the target note
    float target = -1;
    bool anyMidi = false;
    if (p ("midiMode") > 0.5f)
    {
        float bestD = 1e9f;
        for (int nn = 0; nn < 128; ++nn) if (midiHeld[(size_t) nn]) { anyMidi = true; const float d = std::abs (nn - center); if (d < bestD) { bestD = d; target = (float) nn; } }
    }
    if (! anyMidi)
    {
        int custom = 0; for (int i = 0; i < 12; ++i) if (noteParams[(size_t) i]->load() > 0.5f) custom |= 1 << i;
        const int mask = absoluteMask (juce::roundToInt (p ("key")), juce::roundToInt (p ("scale")), custom);
        auto allowed = [&] (int nn) { return mask == 0 || (mask & (1 << ((nn % 12 + 12) % 12))) != 0; };
        if (curNote >= 0 && allowed ((int) curNote) && std::abs (center - curNote) < 0.65f) target = curNote;   // hysteresis
        else
        {
            const int r = juce::roundToInt (center);
            for (int k = 0; k <= 12 && target < 0; ++k)
            {
                const bool up = allowed (r + k), dn = allowed (r - k);
                if (up && dn) target = (float) (std::abs (r + k - center) <= std::abs (r - k - center) ? r + k : r - k);
                else if (up) target = (float) (r + k); else if (dn) target = (float) (r - k);
            }
        }
    }
    if (target != curNote) { curNote = target; heldSeconds = 0; noteSeconds = 0; }
    heldSeconds += dt; noteSeconds += dt;

    // ---- correction amount (Flex‑Tune: big deviations / slides pass through)
    const float dev = center - target;
    // flex 0: correct everything. Higher flex: only notes the singer is already close to get pulled in;
    // larger deviations (scoops, slides, intentional bends) are left alone.
    float w = amount;
    if (flex > 0.001f)
    {
        const float r0 = 0.05f + (1.0f - flex) * 0.95f;     // semitones
        const float x = juce::jlimit (0.0f, 1.0f, (std::abs (dev) - r0) / 0.25f);
        w *= 1.0f - x * x * (3.0f - 2.0f * x);
    }
    float cTarget = -dev * w;
    float vibRemove = (1.0f - vibKeep) * amount;

    // ---- Graph edits override (and optionally silence auto‑correction elsewhere)
    if (tickValid && hasEdit[(size_t) tick].load())
    {
        cTarget = editShift[(size_t) tick].load(); vibRemove = 0.0f; retuneMs = std::min (retuneMs, 15.0f);
    }
    else if (uiMode.load() == 1 && p ("graphAuto") < 0.5f) { cTarget = 0; vibRemove = 0; }

    float tau = retuneMs * 0.001f;
    if (heldSeconds > 0.15f) tau *= 1.0f + humanize * 3.0f;     // Humanize: relax sustained notes
    corr += (cTarget - corr) * (tau < 0.0005f ? 1.0f : 1.0f - std::exp (-dt / tau));

    // ---- added vibrato
    float add = 0;
    const float depth = p ("vibDepth") * 0.01f, delay = p ("vibDelay") * 0.001f;
    if (depth > 0 && noteSeconds > delay)
    {
        vibPhase += juce::MathConstants<float>::twoPi * p ("vibRate") * dt;
        if (vibPhase > juce::MathConstants<float>::twoPi) vibPhase -= juce::MathConstants<float>::twoPi;
        add = depth * std::min (1.0f, (noteSeconds - delay) / 0.3f) * std::sin (vibPhase);
    }

    const float outPitch = m + corr - vibRemove * vib + add + transpose;
    const float ratio = std::pow (2.0f, (outPitch - m) / 12.0f);
    psola.setTarget (period, ratio, formantFactor);

    // ---- auto‑key histogram (favour stable notes)
    const int pc = ((juce::roundToInt (center) % 12) + 12) % 12;
    const float decay = std::exp (-dt / 60.0f);
    for (int i = 0; i < 12; ++i)
    {
        float v = pcHist[(size_t) i].load() * decay;
        if (i == pc) v += dt * (std::abs (vib) < 0.3f ? 1.0f : 0.3f);
        pcHist[(size_t) i].store (v);
    }
    lastMidi = m;
    inMidi.store (m); outMidi.store (outPitch); targetNote.store (target); confidence.store (conf);
    int s1, n1, s2, n2; historyFifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 > 0) historyBuf[(size_t) s1] = { m, outPitch };
    historyFifo.finishedWrite (n1);
}

void VegaTuneProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals nd;
    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        if (msg.isNoteOn()) midiHeld[(size_t) msg.getNoteNumber()] = true;
        else if (msg.isNoteOff()) midiHeld[(size_t) msg.getNoteNumber()] = false;
        else if (msg.isAllNotesOff() || msg.isAllSoundOff()) midiHeld.fill (false);
    }
    if (juce::roundToInt (p ("range")) != curRange || juce::roundToInt (p ("latency")) != curLatencyMode) configure();

    double secs = -1.0; bool playing = false;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            playing = pos->getIsPlaying();
            if (auto t = pos->getTimeInSeconds()) secs = *t;
        }
    playheadSeconds.store (secs); hostPlaying.store (playing);

    const int n = buffer.getNumSamples(), nch = std::min (2, buffer.getNumChannels());
    const int lat = psola.getLatency();
    outGain.setTargetValue (juce::Decibels::decibelsToGain (p ("output")));
    mixS.setTargetValue (p ("mix") * 0.01f);
    bypassS.setTargetValue (p ("bypass") > 0.5f ? 1.0f : 0.0f);
    const int mask = (int) mono.size() - 1, dmask = (1 << 16) - 1;
    float ip = 0, op = 0;
    for (int i = 0; i < n; ++i)
    {
        float inF[2] { buffer.getSample (0, i), nch > 1 ? buffer.getSample (1, i) : buffer.getSample (0, i) };
        float m = 0.5f * (inF[0] + inF[1]);
        mono[(size_t) (monoPos & mask)] = m; ++monoPos;
        if (++hopCounter >= hop) { hopCounter = 0; analyseHop (secs >= 0 ? secs + (double) i / fs : -1.0, playing); }
        float outF[2] { 0, 0 };
        psola.process (inF, outF);
        if (channels < 2) outF[1] = outF[0];
        // latency‑matched dry for Mix and Bypass
        float dry[2];
        for (int c = 0; c < 2; ++c) { dryRing[c][(size_t) dryPos] = inF[c]; dry[c] = dryRing[c][(size_t) ((dryPos - lat) & dmask)]; }
        dryPos = (dryPos + 1) & dmask;
        const float mx = mixS.getNextValue(), g = outGain.getNextValue(), b = bypassS.getNextValue();
        for (int c = 0; c < nch; ++c)
        {
            float y = (dry[c] + mx * (outF[c] - dry[c])) * g;
            y += b * (dry[c] - y);
            buffer.setSample (c, i, y);
            op = std::max (op, std::abs (y));
        }
        ip = std::max (ip, std::abs (m));
    }
    auto amax = [] (std::atomic<float>& a, float v) { float prev = a.load(); while (prev < v && ! a.compare_exchange_weak (prev, v)) {} };
    amax (inPeak, ip); amax (outPeak, op);
}

// ---------------------------------------------------------------- state (incl. graph data)
void VegaTuneProcessor::getStateInformation (juce::MemoryBlock& d)
{
    auto st = apvts.copyState();
    st.setProperty ("uiScale", (double) uiScale.load(), nullptr);
    int last = -1;
    for (int i = 0; i < kMaxTicks; ++i) if (trackMidi[(size_t) i].load() >= 0 || hasEdit[(size_t) i].load()) last = i;
    if (last >= 0)
    {
        juce::MemoryBlock tb ((size_t) (last + 1) * sizeof (float)), eb ((size_t) (last + 1) * sizeof (float));
        auto* t = static_cast<float*> (tb.getData()); auto* e = static_cast<float*> (eb.getData());
        for (int i = 0; i <= last; ++i) { t[i] = trackMidi[(size_t) i].load(); e[i] = hasEdit[(size_t) i].load() ? editShift[(size_t) i].load() : -999.0f; }
        st.setProperty ("graphTrack", tb.toBase64Encoding(), nullptr);
        st.setProperty ("graphEdit", eb.toBase64Encoding(), nullptr);
    }
    if (auto x = st.createXml()) copyXmlToBinary (*x, d);
}
void VegaTuneProcessor::setStateInformation (const void* data, int size)
{
    if (auto x = getXmlFromBinary (data, size))
        if (x->hasTagName (apvts.state.getType()))
        {
            auto t = juce::ValueTree::fromXml (*x);
            uiScale.store ((float) (double) t.getProperty ("uiScale", 1.0));
            clearTrack(); clearEdits();
            juce::MemoryBlock tb, eb;
            if (tb.fromBase64Encoding (t.getProperty ("graphTrack").toString()) && eb.fromBase64Encoding (t.getProperty ("graphEdit").toString()))
            {
                const int cnt = (int) std::min (tb.getSize(), eb.getSize()) / (int) sizeof (float);
                auto* tp = static_cast<const float*> (tb.getData()); auto* ep = static_cast<const float*> (eb.getData());
                for (int i = 0; i < std::min (cnt, kMaxTicks); ++i)
                {
                    trackMidi[(size_t) i].store (tp[i]);
                    if (ep[i] > -998.0f) { editShift[(size_t) i].store (ep[i]); hasEdit[(size_t) i].store (true); }
                }
            }
            t.removeProperty ("graphTrack", nullptr); t.removeProperty ("graphEdit", nullptr);
            apvts.replaceState (t);
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VegaTuneProcessor(); }
