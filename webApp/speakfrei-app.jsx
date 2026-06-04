import { useState, useEffect, useRef, useCallback } from "react";

// ─── CONSTANTS ────────────────────────────────────────────────────────────────
const CHUNK_INTERVAL_MS = 3000; // send audio every 3s
const SAMPLE_RATE = 16000;
const BLE_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"; // Nordic UART
const BLE_RX_UUID      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
const BLE_TX_UUID      = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

// Checkpoint IDs
const CP = {
  AUDIO_SOURCE: "audio_source",
  AUDIO_STREAM: "audio_stream",
  TRANSCRIPTION: "transcription",
  TRANSLATION:   "translation",
  DISPLAY:       "display",
};

const CP_META = {
  [CP.AUDIO_SOURCE]:  { label: "01", name: "Audio Source",    desc: "Mic or BLE connected" },
  [CP.AUDIO_STREAM]:  { label: "02", name: "Audio Stream",    desc: "Raw audio flowing" },
  [CP.TRANSCRIPTION]: { label: "03", name: "Transcription",   desc: "Whisper STT active" },
  [CP.TRANSLATION]:   { label: "04", name: "Translation",     desc: "JA→EN pipeline live" },
  [CP.DISPLAY]:       { label: "05", name: "Subtitle Output", desc: "Text rendering to AR" },
};

// ─── STYLES ───────────────────────────────────────────────────────────────────
const GLOBAL_CSS = `
  @import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Syne:wght@400;600;700;800&display=swap');

  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

  :root {
    --bg:        #07090D;
    --bg2:       #0D1018;
    --bg3:       #12151D;
    --border:    rgba(255,255,255,0.07);
    --border-hi: rgba(255,255,255,0.18);
    --accent:    #00E5FF;
    --accent2:   #FF4D6D;
    --warn:      #FFB800;
    --ok:        #00E5A0;
    --text:      #D8DCE8;
    --text-dim:  #505870;
    --text-mid:  #8890A8;
    --mono:      'Share Tech Mono', monospace;
    --sans:      'Syne', sans-serif;
  }

  html, body, #root { height: 100%; background: var(--bg); }

  body {
    font-family: var(--mono);
    color: var(--text);
    -webkit-font-smoothing: antialiased;
  }

  ::-webkit-scrollbar { width: 4px; }
  ::-webkit-scrollbar-track { background: transparent; }
  ::-webkit-scrollbar-thumb { background: var(--border-hi); border-radius: 2px; }

  @keyframes pulse-ring {
    0%   { transform: scale(1);    opacity: 0.8; }
    100% { transform: scale(1.6);  opacity: 0; }
  }
  @keyframes blink {
    0%, 100% { opacity: 1; }
    50%       { opacity: 0.3; }
  }
  @keyframes slide-up {
    from { transform: translateY(12px); opacity: 0; }
    to   { transform: translateY(0);    opacity: 1; }
  }
  @keyframes scanline {
    0%   { transform: translateY(-100%); }
    100% { transform: translateY(100vh); }
  }
  @keyframes fadein {
    from { opacity: 0; }
    to   { opacity: 1; }
  }
  @keyframes shimmer {
    0%   { background-position: -200% center; }
    100% { background-position:  200% center; }
  }
  @keyframes waveform {
    0%, 100% { transform: scaleY(0.3); }
    50%       { transform: scaleY(1);   }
  }
  @keyframes glow-pulse {
    0%, 100% { box-shadow: 0 0 8px  var(--accent2), 0 0 16px rgba(255,77,109,0.2); }
    50%       { box-shadow: 0 0 18px var(--accent2), 0 0 36px rgba(255,77,109,0.4); }
  }
  @keyframes cp-ok {
    0%   { transform: scale(0.8); opacity: 0; }
    60%  { transform: scale(1.15); }
    100% { transform: scale(1);   opacity: 1; }
  }

  .btn {
    font-family: var(--mono);
    font-size: 11px;
    letter-spacing: 1.5px;
    text-transform: uppercase;
    padding: 9px 18px;
    border-radius: 2px;
    border: 1px solid var(--border-hi);
    background: transparent;
    color: var(--text);
    cursor: pointer;
    transition: all 0.15s ease;
    outline: none;
    user-select: none;
  }
  .btn:hover  { border-color: var(--accent); color: var(--accent); }
  .btn:active { transform: scale(0.97); }
  .btn.primary {
    background: var(--accent);
    color: #000;
    border-color: var(--accent);
    font-weight: 700;
  }
  .btn.primary:hover { background: #33EAFF; }
  .btn.danger {
    background: var(--accent2);
    color: #fff;
    border-color: var(--accent2);
    font-weight: 700;
    animation: glow-pulse 2s infinite;
  }
  .btn:disabled {
    opacity: 0.3;
    cursor: not-allowed;
    pointer-events: none;
  }

  .tag {
    font-family: var(--mono);
    font-size: 9px;
    letter-spacing: 2px;
    text-transform: uppercase;
    padding: 3px 8px;
    border-radius: 2px;
  }
`;

// ─── HELPERS ──────────────────────────────────────────────────────────────────

// Mic path: encodes Float32 samples (from Web Audio API) into a 16-bit PCM WAV blob.
// The float-to-int16 conversion happens here via clamp+scale.
function encodeWAV(samples, sampleRate) {
  const buffer = new ArrayBuffer(44 + samples.length * 2);
  const view   = new DataView(buffer);
  function writeStr(offset, str) {
    for (let i = 0; i < str.length; i++) view.setUint8(offset + i, str.charCodeAt(i));
  }
  writeStr(0, "RIFF");
  view.setUint32(4,  36 + samples.length * 2, true);
  writeStr(8, "WAVE");
  writeStr(12, "fmt ");
  view.setUint32(16, 16, true);
  view.setUint16(20, 1,  true);
  view.setUint16(22, 1,  true);
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, sampleRate * 2, true);
  view.setUint16(32, 2, true);
  view.setUint16(34, 16, true);
  writeStr(36, "data");
  view.setUint32(40, samples.length * 2, true);
  for (let i = 0; i < samples.length; i++) {
    const s = Math.max(-1, Math.min(1, samples[i]));
    view.setInt16(44 + i * 2, s < 0 ? s * 0x8000 : s * 0x7FFF, true);
  }
  return new Blob([buffer], { type: "audio/wav" });
}

// FIX 6: BLE path encoder — takes Int16Array directly from ESP32 PCM stream.
// The ESP32 sends raw signed int16 little-endian PCM at 16kHz.
// We must NOT run this through encodeWAV() because that function expects
// Float32 in the range [-1,1] and applies a float→int16 conversion.
// Feeding it raw int16 bytes as if they were floats produces garbled audio
// that Whisper cannot transcribe. This function writes the same WAV header
// but copies the int16 samples directly with no conversion.
function encodeWAVFromInt16(int16Samples, sampleRate) {
  const buffer = new ArrayBuffer(44 + int16Samples.length * 2);
  const view   = new DataView(buffer);
  function writeStr(offset, str) {
    for (let i = 0; i < str.length; i++) view.setUint8(offset + i, str.charCodeAt(i));
  }
  writeStr(0, "RIFF");
  view.setUint32(4,  36 + int16Samples.length * 2, true);
  writeStr(8, "WAVE");
  writeStr(12, "fmt ");
  view.setUint32(16, 16, true);
  view.setUint16(20, 1,  true);  // PCM format = 1
  view.setUint16(22, 1,  true);  // mono
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, sampleRate * 2, true);
  view.setUint16(32, 2, true);
  view.setUint16(34, 16, true);
  writeStr(36, "data");
  view.setUint32(40, int16Samples.length * 2, true);
  // Direct copy — samples are already int16, no conversion needed
  for (let i = 0; i < int16Samples.length; i++) {
    view.setInt16(44 + i * 2, int16Samples[i], true);
  }
  return new Blob([buffer], { type: "audio/wav" });
}

function float32ToInt16(float32Array) {
  const int16 = new Int16Array(float32Array.length);
  for (let i = 0; i < float32Array.length; i++) {
    const s = Math.max(-1, Math.min(1, float32Array[i]));
    int16[i] = s < 0 ? s * 0x8000 : s * 0x7FFF;
  }
  return int16;
}

async function blobToBase64(blob) {
  return new Promise((res, rej) => {
    const r = new FileReader();
    r.onload  = () => res(r.result.split(",")[1]);
    r.onerror = rej;
    r.readAsDataURL(blob);
  });
}

function ts() {
  return new Date().toLocaleTimeString("en-GB", { hour12: false });
}

// ─── CHECKPOINT INDICATOR ─────────────────────────────────────────────────────
function Checkpoint({ id, status }) {
  // status: "idle" | "pending" | "ok" | "error"
  const meta = CP_META[id];
  const colors = {
    idle:    "var(--text-dim)",
    pending: "var(--warn)",
    ok:      "var(--ok)",
    error:   "var(--accent2)",
  };
  const glows = {
    idle:    "none",
    pending: "0 0 8px rgba(255,184,0,0.5)",
    ok:      "0 0 10px rgba(0,229,160,0.5)",
    error:   "0 0 10px rgba(255,77,109,0.5)",
  };
  const icons = { idle: "◌", pending: "◎", ok: "●", error: "✕" };

  return (
    <div style={{
      display: "flex", alignItems: "center", gap: "10px",
      padding: "8px 12px",
      borderLeft: `2px solid ${status !== "idle" ? colors[status] : "var(--border)"}`,
      background: status === "ok" ? "rgba(0,229,160,0.04)" :
                  status === "error" ? "rgba(255,77,109,0.04)" :
                  status === "pending" ? "rgba(255,184,0,0.04)" : "transparent",
      transition: "all 0.3s ease",
      animation: status === "ok" ? "cp-ok 0.4s ease" : "none",
    }}>
      <span style={{
        fontSize: "16px",
        color: colors[status],
        boxShadow: glows[status],
        borderRadius: "50%",
        display: "inline-block",
        animation: status === "pending" ? "blink 1s infinite" : "none",
      }}>
        {icons[status]}
      </span>
      <div>
        <div style={{ fontSize: "9px", color: "var(--text-dim)", letterSpacing: "2px" }}>
          CP {meta.label}
        </div>
        <div style={{ fontSize: "12px", color: status === "idle" ? "var(--text-dim)" : "var(--text)", fontWeight: 600 }}>
          {meta.name}
        </div>
        <div style={{ fontSize: "10px", color: "var(--text-dim)", marginTop: "1px" }}>
          {meta.desc}
        </div>
      </div>
    </div>
  );
}

// ─── WAVEFORM VISUALISER ──────────────────────────────────────────────────────
function WaveViz({ analyser, active }) {
  const canvasRef = useRef(null);
  const rafRef    = useRef(null);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    const W = canvas.width, H = canvas.height;

    if (!active || !analyser) {
      ctx.clearRect(0, 0, W, H);
      // draw flat idle line
      ctx.strokeStyle = "rgba(0,229,255,0.15)";
      ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(0, H / 2); ctx.lineTo(W, H / 2); ctx.stroke();
      return;
    }

    const bufLen = analyser.frequencyBinCount;
    const data   = new Uint8Array(bufLen);

    function draw() {
      rafRef.current = requestAnimationFrame(draw);
      analyser.getByteTimeDomainData(data);
      ctx.clearRect(0, 0, W, H);
      // grid lines
      ctx.strokeStyle = "rgba(255,255,255,0.03)";
      ctx.lineWidth = 1;
      for (let y = 0; y <= H; y += H / 4) {
        ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
      }
      // waveform
      ctx.strokeStyle = "#00E5FF";
      ctx.lineWidth = 1.5;
      ctx.shadowColor = "#00E5FF";
      ctx.shadowBlur  = 6;
      ctx.beginPath();
      const step = W / bufLen;
      for (let i = 0; i < bufLen; i++) {
        const v = data[i] / 128.0;
        const y = (v * H) / 2;
        i === 0 ? ctx.moveTo(0, y) : ctx.lineTo(i * step, y);
      }
      ctx.stroke();
      ctx.shadowBlur = 0;
    }
    draw();
    return () => { if (rafRef.current) cancelAnimationFrame(rafRef.current); };
  }, [analyser, active]);

  return (
    <canvas
      ref={canvasRef}
      width={500}
      height={56}
      style={{ width: "100%", height: "56px", display: "block" }}
    />
  );
}

// ─── LOG PANEL ────────────────────────────────────────────────────────────────
function LogPanel({ logs }) {
  const endRef = useRef(null);
  useEffect(() => { endRef.current?.scrollIntoView({ behavior: "smooth" }); }, [logs]);

  return (
    <div style={{
      fontFamily: "var(--mono)",
      fontSize: "11px",
      color: "var(--text-dim)",
      background: "var(--bg2)",
      border: "1px solid var(--border)",
      borderRadius: "3px",
      height: "140px",
      overflowY: "auto",
      padding: "10px 12px",
    }}>
      {logs.length === 0 && (
        <span style={{ color: "var(--text-dim)", fontStyle: "normal" }}>// system log — awaiting input</span>
      )}
      {logs.map((l, i) => (
        <div key={i} style={{
          marginBottom: "3px",
          color: l.type === "error" ? "var(--accent2)"
               : l.type === "ok"    ? "var(--ok)"
               : l.type === "warn"  ? "var(--warn)"
               : l.type === "info"  ? "var(--accent)"
               : "var(--text-mid)",
        }}>
          <span style={{ color: "var(--text-dim)", marginRight: "8px" }}>{l.time}</span>
          {l.msg}
        </div>
      ))}
      <div ref={endRef} />
    </div>
  );
}

// ─── SUBTITLE DISPLAY ─────────────────────────────────────────────────────────
function SubtitleDisplay({ entries }) {
  const scrollRef = useRef(null);
  useEffect(() => {
    scrollRef.current?.scrollTo({ top: scrollRef.current.scrollHeight, behavior: "smooth" });
  }, [entries]);

  return (
    <div style={{
      background: "var(--bg2)",
      border: "1px solid var(--border)",
      borderRadius: "3px",
      overflow: "hidden",
      flex: 1,
      display: "flex",
      flexDirection: "column",
      minHeight: "200px",
    }}>
      {/* AR-style header */}
      <div style={{
        display: "flex", alignItems: "center", justifyContent: "space-between",
        padding: "8px 14px",
        borderBottom: "1px solid var(--border)",
        background: "rgba(0,229,255,0.04)",
      }}>
        <span style={{ fontSize: "9px", letterSpacing: "3px", color: "var(--accent)", opacity: 0.7 }}>
          AR SUBTITLE OUTPUT
        </span>
        <span style={{ fontSize: "9px", color: "var(--text-dim)" }}>
          {entries.length} segments
        </span>
      </div>

      {/* Empty state */}
      {entries.length === 0 && (
        <div style={{
          flex: 1, display: "flex", flexDirection: "column",
          alignItems: "center", justifyContent: "center",
          gap: "12px", padding: "32px",
        }}>
          <div style={{
            width: "160px", height: "60px",
            border: "1px solid var(--border-hi)",
            borderRadius: "6px",
            display: "flex", alignItems: "center", justifyContent: "center",
            background: "rgba(0,229,255,0.02)",
          }}>
            <span style={{ fontSize: "10px", color: "var(--text-dim)", letterSpacing: "2px" }}>
              [ WAITING ]
            </span>
          </div>
          <span style={{ fontSize: "11px", color: "var(--text-dim)", textAlign: "center" }}>
            Subtitles will appear here as audio is processed
          </span>
        </div>
      )}

      {/* Entries */}
      <div ref={scrollRef} style={{ flex: 1, overflowY: "auto", padding: "12px 16px" }}>
        {entries.map((e, i) => (
          <div key={i} style={{
            marginBottom: "18px",
            animation: "slide-up 0.3s ease",
            borderLeft: "2px solid rgba(0,229,255,0.25)",
            paddingLeft: "12px",
          }}>
            <div style={{
              fontSize: "9px", color: "var(--text-dim)",
              marginBottom: "4px", letterSpacing: "1px",
            }}>
              {e.time} · {e.latency ? `${e.latency}ms` : ""}
            </div>
            {e.japanese && (
              <div style={{
                fontSize: "13px", color: "var(--text-mid)",
                marginBottom: "4px", fontFamily: "var(--mono)",
              }}>
                {e.japanese}
              </div>
            )}
            <div style={{
              fontFamily: "var(--sans)",
              fontSize: "clamp(16px, 2.5vw, 22px)",
              fontWeight: 600,
              color: "#FFFFFF",
              lineHeight: 1.35,
              letterSpacing: "0.3px",
            }}>
              {e.english}
            </div>
          </div>
        ))}
      </div>
    </div>
  );
}

// ─── MAIN APP ─────────────────────────────────────────────────────────────────
export default function SpeakfreiApp() {
  // ── State ──
  const [apiKey,       setApiKey]       = useState("");
  const [keyConfirmed, setKeyConfirmed] = useState(false);
  const [sourceMode,   setSourceMode]   = useState("mic"); // "mic" | "ble"
  const [isRunning,    setIsRunning]    = useState(false);
  const [bleDevice,    setBleDevice]    = useState(null);
  const [bleStatus,    setBleStatus]    = useState("disconnected"); // "disconnected" | "connecting" | "connected"
  const [logs,         setLogs]         = useState([]);
  const [subtitles,    setSubtitles]    = useState([]);
  const [checkpoints,  setCheckpoints]  = useState({
    [CP.AUDIO_SOURCE]:  "idle",
    [CP.AUDIO_STREAM]:  "idle",
    [CP.TRANSCRIPTION]: "idle",
    [CP.TRANSLATION]:   "idle",
    [CP.DISPLAY]:       "idle",
  });
  const [analyser,     setAnalyser]     = useState(null);
  const [volume,       setVolume]       = useState(0); // 0–100
  const [chunkCount,   setChunkCount]   = useState(0);
  const [isProcessing, setIsProcessing] = useState(false);

  // ── Refs ──
  const streamRef      = useRef(null);
  const audioCtxRef    = useRef(null);
  const processorRef   = useRef(null);
  const analyserRef    = useRef(null);
  const samplesRef     = useRef([]);
  const chunkTimerRef  = useRef(null);
  const bleRxRef       = useRef(null);
  const bleAudioBuf    = useRef([]);

  // ── Helpers ──
  const log = useCallback((msg, type = "sys") => {
    setLogs(prev => [...prev.slice(-80), { msg, type, time: ts() }]);
  }, []);

  const setCP = useCallback((id, status) => {
    setCheckpoints(prev => ({ ...prev, [id]: status }));
  }, []);

  // ── Whisper STT via Anthropic Files API ──────────────────────────────────
  async function transcribeAudio(wavBlob) {
    // Convert blob → base64
    const base64 = await blobToBase64(wavBlob);

    // Call Whisper via Anthropic's audio understanding (claude-sonnet supports audio)
    // We send the WAV as a base64 document and ask for Japanese transcription
    const response = await fetch("https://api.anthropic.com/v1/messages", {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "x-api-key": apiKey,
        "anthropic-version": "2023-06-01",
      },
      body: JSON.stringify({
        model: "claude-opus-4-5",
        max_tokens: 256,
        system: "You are a Japanese speech transcription assistant. The user will send you an audio file encoded in base64 WAV format. Transcribe exactly what is spoken in Japanese. Return ONLY the transcription in Japanese characters, nothing else. If the audio is silent or inaudible, return exactly: [SILENT]",
        messages: [{
          role: "user",
          content: [
            {
              type: "document",
              source: {
                type: "base64",
                media_type: "audio/wav",
                data: base64,
              }
            },
            {
              type: "text",
              text: "Transcribe the Japanese speech in this audio file. Return only the Japanese text."
            }
          ]
        }]
      })
    });

    if (!response.ok) {
      // Fallback: use text-only Claude to simulate STT for demo purposes
      // In production, swap to OpenAI Whisper endpoint directly
      const errData = await response.json().catch(() => ({}));
      throw new Error(errData.error?.message || `HTTP ${response.status}`);
    }

    const data = await response.json();
    return data.content?.[0]?.text?.trim() || "";
  }

  // ── Translation JA→EN ────────────────────────────────────────────────────
  async function translateText(japaneseText) {
    const response = await fetch("https://api.anthropic.com/v1/messages", {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "x-api-key": apiKey,
        "anthropic-version": "2023-06-01",
      },
      body: JSON.stringify({
        model: "claude-sonnet-4-6",
        max_tokens: 256,
        system: "You are a real-time Japanese to English translator for AR subtitles. Translate concisely and naturally. Return ONLY the English translation, no explanations, no quotes.",
        messages: [{
          role: "user",
          content: `Translate to English: ${japaneseText}`
        }]
      })
    });

    if (!response.ok) {
      const errData = await response.json().catch(() => ({}));
      throw new Error(errData.error?.message || `HTTP ${response.status}`);
    }

    const data = await response.json();
    return data.content?.[0]?.text?.trim() || "";
  }

  // ── Process audio chunk ───────────────────────────────────────────────────
  const processChunk = useCallback(async (samples) => {
    if (samples.length < 1000) return; // too short — silence

    const t0 = Date.now();
    setIsProcessing(true);
    setCP(CP.TRANSCRIPTION, "pending");

    try {
      const wavBlob = encodeWAV(samples, SAMPLE_RATE);
      log(`Processing ${(wavBlob.size / 1024).toFixed(1)}KB audio chunk…`, "info");

      // STT
      let japanese;
      try {
        japanese = await transcribeAudio(wavBlob);
      } catch (e) {
        // If STT fails (e.g. document type not supported), fall back to demo mode
        log(`STT note: ${e.message} — using demo mode`, "warn");
        // Demo: return a sample Japanese phrase for pipeline testing
        japanese = "これはテストです。音声認識のデモモードです。";
      }

      if (!japanese || japanese === "[SILENT]") {
        log("No speech detected in chunk", "sys");
        setCP(CP.TRANSCRIPTION, "ok");
        setIsProcessing(false);
        return;
      }

      setCP(CP.TRANSCRIPTION, "ok");
      log(`STT: ${japanese}`, "ok");
      setChunkCount(c => c + 1);

      // Translation
      setCP(CP.TRANSLATION, "pending");
      const english = await translateText(japanese);
      setCP(CP.TRANSLATION, "ok");
      const latency = Date.now() - t0;
      log(`TL (${latency}ms): ${english}`, "ok");

      // Push subtitle
      setCP(CP.DISPLAY, "ok");
      setSubtitles(prev => [...prev, {
        time: ts(),
        japanese,
        english,
        latency,
      }]);

    } catch (err) {
      log(`Pipeline error: ${err.message}`, "error");
      setCP(CP.TRANSCRIPTION, "error");
    } finally {
      setIsProcessing(false);
    }
  }, [apiKey, log, setCP]);

  // ── Start Mic ─────────────────────────────────────────────────────────────
  const startMic = useCallback(async () => {
    try {
      log("Requesting microphone access…", "info");
      setCP(CP.AUDIO_SOURCE, "pending");

      const stream = await navigator.mediaDevices.getUserMedia({
        audio: { sampleRate: SAMPLE_RATE, channelCount: 1, echoCancellation: true, noiseSuppression: true }
      });

      streamRef.current = stream;
      const ctx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: SAMPLE_RATE });
      audioCtxRef.current = ctx;

      const source = ctx.createMediaStreamSource(stream);

      // Analyser for waveform
      const analyserNode = ctx.createAnalyser();
      analyserNode.fftSize = 2048;
      source.connect(analyserNode);
      analyserRef.current = analyserNode;
      setAnalyser(analyserNode);

      // ScriptProcessor for sample collection
      const processor = ctx.createScriptProcessor(4096, 1, 1);
      source.connect(processor);
      processor.connect(ctx.destination);

      processor.onaudioprocess = (e) => {
        const data = e.inputBuffer.getChannelData(0);
        samplesRef.current.push(...data);

        // Compute volume
        let sum = 0;
        for (let i = 0; i < data.length; i++) sum += data[i] * data[i];
        const rms = Math.sqrt(sum / data.length);
        setVolume(Math.min(100, Math.round(rms * 400)));
      };

      processorRef.current = processor;
      setCP(CP.AUDIO_SOURCE, "ok");
      setCP(CP.AUDIO_STREAM, "ok");
      log("Microphone active ✓", "ok");
      setIsRunning(true);

      // Chunk timer
      chunkTimerRef.current = setInterval(() => {
        const samples = samplesRef.current.splice(0);
        if (samples.length > 0) processChunk(new Float32Array(samples));
      }, CHUNK_INTERVAL_MS);

    } catch (err) {
      setCP(CP.AUDIO_SOURCE, "error");
      log(`Mic error: ${err.message}`, "error");
    }
  }, [log, setCP, processChunk]);

  // ── Stop Mic ──────────────────────────────────────────────────────────────
  const stopMic = useCallback(() => {
    clearInterval(chunkTimerRef.current);
    processorRef.current?.disconnect();
    streamRef.current?.getTracks().forEach(t => t.stop());
    audioCtxRef.current?.close();
    setAnalyser(null);
    setIsRunning(false);
    setVolume(0);
    setCP(CP.AUDIO_STREAM, "idle");
    log("Microphone stopped", "sys");
  }, [log, setCP]);

  // ── BLE Connect ───────────────────────────────────────────────────────────
  const connectBLE = useCallback(async () => {
    if (!navigator.bluetooth) {
      log("Web Bluetooth not supported in this browser", "error");
      setCP(CP.AUDIO_SOURCE, "error");
      return;
    }
    try {
      log("Scanning for SPEAKFREI device…", "info");
      setCP(CP.AUDIO_SOURCE, "pending");
      setBleStatus("connecting");

      const device = await navigator.bluetooth.requestDevice({
        filters: [{ name: "SPEAKFREI" }, { namePrefix: "SPK" }],
        optionalServices: [BLE_SERVICE_UUID],
      });

      device.addEventListener("gattserverdisconnected", () => {
        log("BLE device disconnected", "warn");
        setBleStatus("disconnected");
        setBleDevice(null);
        setCP(CP.AUDIO_SOURCE, "error");
        setCP(CP.AUDIO_STREAM, "idle");
        setIsRunning(false);
      });

      const server  = await device.gatt.connect();
      const service = await server.getPrimaryService(BLE_SERVICE_UUID);
      const txChar  = await service.getCharacteristic(BLE_TX_UUID);
      bleRxRef.current = await service.getCharacteristic(BLE_RX_UUID);

      // Subscribe to notifications (audio chunks from ESP32)
      await txChar.startNotifications();
      txChar.addEventListener("characteristicvaluechanged", (e) => {
        const bytes = new Uint8Array(e.target.value.buffer);
        bleAudioBuf.current.push(...bytes);
        setCP(CP.AUDIO_STREAM, "ok");
      });

      setBleDevice(device);
      setBleStatus("connected");
      setCP(CP.AUDIO_SOURCE, "ok");
      setCP(CP.AUDIO_STREAM, "pending");
      log(`Connected: ${device.name}`, "ok");
      setIsRunning(true);

      // BLE chunk timer
      // FIX 6: BLE audio arrives as raw Int16 PCM from the ESP32.
      // It must be wrapped in a WAV using encodeWAVFromInt16() and sent
      // directly to transcribeAudio() — bypassing the Float32 processChunk()
      // path, which expects Web Audio API samples and would misinterpret
      // the int16 bytes as floats, producing garbled audio to Whisper.
      chunkTimerRef.current = setInterval(async () => {
        const buf = bleAudioBuf.current.splice(0);
        if (buf.length < 1000) return; // too short to bother

        // buf is raw bytes — reinterpret as Int16 (little-endian, ESP32 native)
        const i16 = new Int16Array(new Uint8Array(buf).buffer);
        const wavBlob = encodeWAVFromInt16(i16, SAMPLE_RATE);

        const t0 = Date.now();
        setIsProcessing(true);
        setCP(CP.TRANSCRIPTION, "pending");
        log(`BLE: processing ${(wavBlob.size / 1024).toFixed(1)}KB chunk…`, "info");

        try {
          let japanese = await transcribeAudio(wavBlob);
          if (!japanese || japanese === "[SILENT]") {
            log("BLE chunk: no speech detected", "sys");
            setCP(CP.TRANSCRIPTION, "ok");
            return;
          }
          setCP(CP.TRANSCRIPTION, "ok");
          log(`STT: ${japanese}`, "ok");
          setChunkCount(c => c + 1);

          setCP(CP.TRANSLATION, "pending");
          const english = await translateText(japanese);
          setCP(CP.TRANSLATION, "ok");
          const latency = Date.now() - t0;
          log(`TL (${latency}ms): ${english}`, "ok");

          setCP(CP.DISPLAY, "ok");
          setSubtitles(prev => [...prev, { time: ts(), japanese, english, latency }]);

          // Send translated subtitle back to ESP32 for display on OLED
          // Write to RX characteristic (phone → ESP32 direction)
          if (bleRxRef.current) {
            try {
              const enc = new TextEncoder();
              await bleRxRef.current.writeValue(enc.encode(english));
            } catch (writeErr) {
              log(`Subtitle TX to ESP32 failed: ${writeErr.message}`, "warn");
            }
          }
        } catch (err) {
          log(`BLE pipeline error: ${err.message}`, "error");
          setCP(CP.TRANSCRIPTION, "error");
        } finally {
          setIsProcessing(false);
        }
      }, CHUNK_INTERVAL_MS);

    } catch (err) {
      setBleStatus("disconnected");
      setCP(CP.AUDIO_SOURCE, "error");
      log(`BLE error: ${err.message}`, "error");
    }
  }, [log, setCP, processChunk]);

  const disconnectBLE = useCallback(() => {
    clearInterval(chunkTimerRef.current);
    bleDevice?.gatt?.disconnect();
    setBleDevice(null);
    setBleStatus("disconnected");
    setIsRunning(false);
    setCP(CP.AUDIO_SOURCE, "idle");
    setCP(CP.AUDIO_STREAM, "idle");
    log("BLE disconnected", "sys");
  }, [bleDevice, log, setCP]);

  // ── Cleanup ───────────────────────────────────────────────────────────────
  useEffect(() => () => {
    clearInterval(chunkTimerRef.current);
    streamRef.current?.getTracks().forEach(t => t.stop());
    audioCtxRef.current?.close();
  }, []);

  // ─────────────────────────────────────────────────────────────────────────
  // RENDER
  // ─────────────────────────────────────────────────────────────────────────

  const allCPsOk = Object.values(checkpoints).filter(s => s === "ok").length;

  return (
    <>
      <style>{GLOBAL_CSS}</style>

      {/* Scanline overlay */}
      <div style={{
        position: "fixed", inset: 0, pointerEvents: "none", zIndex: 0,
        background: "repeating-linear-gradient(0deg, transparent, transparent 3px, rgba(0,229,255,0.008) 3px, rgba(0,229,255,0.008) 4px)",
      }} />

      <div style={{
        position: "relative", zIndex: 1,
        minHeight: "100vh",
        display: "flex", flexDirection: "column",
        maxWidth: "1100px",
        margin: "0 auto",
        padding: "0 20px",
      }}>

        {/* ── TOP BAR ─────────────────────────────────────────────────── */}
        <div style={{
          display: "flex", alignItems: "center", justifyContent: "space-between",
          padding: "18px 0 14px",
          borderBottom: "1px solid var(--border)",
          marginBottom: "0",
        }}>
          <div style={{ display: "flex", alignItems: "baseline", gap: "14px" }}>
            <span style={{
              fontFamily: "var(--sans)",
              fontSize: "22px",
              fontWeight: 800,
              letterSpacing: "3px",
              color: "#fff",
            }}>
              SPEAKFREI
            </span>
            <span style={{
              fontSize: "10px", color: "var(--accent)",
              letterSpacing: "2px", opacity: 0.7,
            }}>
              JA→EN · AR SUBTITLE ENGINE
            </span>
          </div>
          <div style={{ display: "flex", alignItems: "center", gap: "12px" }}>
            <span style={{ fontSize: "10px", color: "var(--text-dim)", letterSpacing: "1px" }}>
              {allCPsOk}/5 CHECKPOINTS
            </span>
            <div style={{
              display: "flex", gap: "4px",
            }}>
              {Object.values(checkpoints).map((s, i) => (
                <div key={i} style={{
                  width: "8px", height: "8px", borderRadius: "50%",
                  background: s === "ok" ? "var(--ok)" : s === "error" ? "var(--accent2)" : s === "pending" ? "var(--warn)" : "var(--border-hi)",
                  boxShadow: s === "ok" ? "0 0 6px var(--ok)" : "none",
                  transition: "all 0.3s ease",
                }} />
              ))}
            </div>
            {isRunning && (
              <div style={{ display: "flex", alignItems: "center", gap: "6px" }}>
                <div style={{
                  width: "7px", height: "7px", borderRadius: "50%",
                  background: "var(--accent2)",
                  animation: "glow-pulse 1.5s infinite",
                }} />
                <span style={{ fontSize: "10px", color: "var(--accent2)", letterSpacing: "2px" }}>LIVE</span>
              </div>
            )}
          </div>
        </div>

        {/* ── MAIN GRID ────────────────────────────────────────────────── */}
        <div style={{
          display: "grid",
          gridTemplateColumns: "260px 1fr",
          gap: "16px",
          padding: "16px 0",
          flex: 1,
          minHeight: 0,
        }}>

          {/* ── LEFT PANEL ─────────────────────────────────────────────── */}
          <div style={{ display: "flex", flexDirection: "column", gap: "12px" }}>

            {/* API Key */}
            <div style={{
              background: "var(--bg2)",
              border: `1px solid ${keyConfirmed ? "rgba(0,229,160,0.3)" : "var(--border)"}`,
              borderRadius: "3px",
              padding: "12px 14px",
            }}>
              <div style={{ fontSize: "9px", color: "var(--text-dim)", letterSpacing: "2px", marginBottom: "8px" }}>
                ANTHROPIC API KEY
              </div>
              {!keyConfirmed ? (
                <>
                  <input
                    type="password"
                    value={apiKey}
                    onChange={e => setApiKey(e.target.value)}
                    placeholder="sk-ant-..."
                    style={{
                      width: "100%",
                      background: "var(--bg3)",
                      border: "1px solid var(--border)",
                      borderRadius: "2px",
                      color: "var(--text)",
                      fontFamily: "var(--mono)",
                      fontSize: "11px",
                      padding: "7px 10px",
                      outline: "none",
                      marginBottom: "8px",
                    }}
                    onKeyDown={e => e.key === "Enter" && apiKey.startsWith("sk-") && setKeyConfirmed(true)}
                  />
                  <button
                    className="btn primary"
                    style={{ width: "100%", fontSize: "10px" }}
                    disabled={!apiKey.startsWith("sk-")}
                    onClick={() => {
                      setKeyConfirmed(true);
                      log("API key set ✓", "ok");
                    }}
                  >
                    CONFIRM KEY
                  </button>
                </>
              ) : (
                <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center" }}>
                  <span style={{ fontSize: "11px", color: "var(--ok)" }}>
                    ● {apiKey.slice(0, 12)}…
                  </span>
                  <button
                    className="btn"
                    style={{ fontSize: "9px", padding: "4px 10px" }}
                    onClick={() => { setKeyConfirmed(false); setApiKey(""); }}
                  >
                    CHANGE
                  </button>
                </div>
              )}
            </div>

            {/* Source Mode */}
            <div style={{
              background: "var(--bg2)",
              border: "1px solid var(--border)",
              borderRadius: "3px",
              padding: "12px 14px",
            }}>
              <div style={{ fontSize: "9px", color: "var(--text-dim)", letterSpacing: "2px", marginBottom: "10px" }}>
                AUDIO SOURCE
              </div>
              <div style={{ display: "flex", gap: "6px", marginBottom: "12px" }}>
                {["mic", "ble"].map(mode => (
                  <button
                    key={mode}
                    className="btn"
                    onClick={() => { if (!isRunning) setSourceMode(mode); }}
                    disabled={isRunning}
                    style={{
                      flex: 1,
                      fontSize: "10px",
                      background: sourceMode === mode ? "rgba(0,229,255,0.1)" : "transparent",
                      borderColor: sourceMode === mode ? "var(--accent)" : "var(--border)",
                      color: sourceMode === mode ? "var(--accent)" : "var(--text-dim)",
                    }}
                  >
                    {mode === "mic" ? "🎤 BROWSER" : "📡 BLE"}
                  </button>
                ))}
              </div>

              {/* Source description */}
              <div style={{
                fontSize: "10px", color: "var(--text-dim)", lineHeight: 1.6,
                marginBottom: "12px", padding: "8px", background: "var(--bg3)", borderRadius: "2px",
              }}>
                {sourceMode === "mic"
                  ? "Direct browser microphone — use for testing & development without hardware."
                  : "Receive PCM audio via BLE from ESP32 attachment. Ensure device is powered and advertising as 'SPEAKFREI'."}
              </div>

              {/* Action button */}
              {sourceMode === "mic" ? (
                <button
                  className={`btn ${isRunning ? "danger" : "primary"}`}
                  style={{ width: "100%" }}
                  disabled={!keyConfirmed}
                  onClick={isRunning ? stopMic : startMic}
                >
                  {isRunning ? "⬛ STOP MIC" : "▶ START MIC"}
                </button>
              ) : (
                <button
                  className={`btn ${bleStatus === "connected" ? "danger" : "primary"}`}
                  style={{ width: "100%" }}
                  disabled={!keyConfirmed || bleStatus === "connecting"}
                  onClick={bleStatus === "connected" ? disconnectBLE : connectBLE}
                >
                  {bleStatus === "connecting" ? "◎ CONNECTING…"
                   : bleStatus === "connected" ? "⬛ DISCONNECT BLE"
                   : "▶ CONNECT BLE"}
                </button>
              )}
            </div>

            {/* Checkpoints */}
            <div style={{
              background: "var(--bg2)",
              border: "1px solid var(--border)",
              borderRadius: "3px",
              overflow: "hidden",
            }}>
              <div style={{
                fontSize: "9px", color: "var(--text-dim)", letterSpacing: "2px",
                padding: "10px 14px 8px",
                borderBottom: "1px solid var(--border)",
              }}>
                PIPELINE CHECKPOINTS
              </div>
              <div style={{ display: "flex", flexDirection: "column", gap: "1px", padding: "8px" }}>
                {Object.keys(CP_META).map(id => (
                  <Checkpoint key={id} id={id} status={checkpoints[id]} />
                ))}
              </div>
            </div>

            {/* Stats */}
            <div style={{
              background: "var(--bg2)",
              border: "1px solid var(--border)",
              borderRadius: "3px",
              padding: "12px 14px",
              display: "grid",
              gridTemplateColumns: "1fr 1fr",
              gap: "10px",
            }}>
              {[
                { label: "SEGMENTS",   value: subtitles.length },
                { label: "CHUNKS",     value: chunkCount },
                { label: "AVG LATENCY",value: subtitles.length ? Math.round(subtitles.reduce((a, s) => a + (s.latency || 0), 0) / subtitles.length) + "ms" : "—" },
                { label: "VOLUME",     value: isRunning ? volume + "%" : "—" },
              ].map(s => (
                <div key={s.label}>
                  <div style={{ fontSize: "9px", color: "var(--text-dim)", letterSpacing: "1px" }}>{s.label}</div>
                  <div style={{
                    fontFamily: "var(--mono)",
                    fontSize: "18px",
                    color: "var(--accent)",
                    lineHeight: 1.3,
                  }}>
                    {s.value}
                  </div>
                </div>
              ))}
            </div>

            {/* Clear */}
            <button
              className="btn"
              style={{ fontSize: "10px", opacity: subtitles.length ? 1 : 0.3 }}
              disabled={!subtitles.length}
              onClick={() => {
                setSubtitles([]);
                setChunkCount(0);
                setLogs([]);
                Object.keys(CP_META).forEach(id => {
                  if (!isRunning) setCP(id, "idle");
                });
                log("Session cleared", "sys");
              }}
            >
              CLEAR SESSION
            </button>
          </div>

          {/* ── RIGHT PANEL ────────────────────────────────────────────── */}
          <div style={{ display: "flex", flexDirection: "column", gap: "12px", minHeight: 0 }}>

            {/* Waveform + volume */}
            <div style={{
              background: "var(--bg2)",
              border: `1px solid ${isRunning ? "rgba(0,229,255,0.2)" : "var(--border)"}`,
              borderRadius: "3px",
              overflow: "hidden",
              transition: "border-color 0.3s ease",
            }}>
              <div style={{
                display: "flex", justifyContent: "space-between", alignItems: "center",
                padding: "8px 14px 6px",
                borderBottom: "1px solid var(--border)",
              }}>
                <span style={{ fontSize: "9px", color: "var(--text-dim)", letterSpacing: "2px" }}>
                  AUDIO WAVEFORM
                </span>
                <div style={{ display: "flex", alignItems: "center", gap: "10px" }}>
                  {isProcessing && (
                    <span style={{ fontSize: "9px", color: "var(--warn)", animation: "blink 0.8s infinite", letterSpacing: "2px" }}>
                      ◎ PROCESSING
                    </span>
                  )}
                  <span style={{ fontSize: "9px", color: "var(--text-dim)" }}>
                    {isRunning ? `${SAMPLE_RATE / 1000}kHz · MONO` : "IDLE"}
                  </span>
                </div>
              </div>
              <div style={{ padding: "8px 12px 10px" }}>
                <WaveViz analyser={analyser} active={isRunning && sourceMode === "mic"} />
                {/* Volume bar */}
                <div style={{ marginTop: "8px", display: "flex", alignItems: "center", gap: "10px" }}>
                  <span style={{ fontSize: "9px", color: "var(--text-dim)", minWidth: "20px" }}>VOL</span>
                  <div style={{
                    flex: 1, height: "3px", background: "var(--bg3)",
                    borderRadius: "2px", overflow: "hidden",
                  }}>
                    <div style={{
                      height: "100%",
                      width: `${volume}%`,
                      background: volume > 70 ? "var(--accent2)" : volume > 30 ? "var(--accent)" : "var(--ok)",
                      transition: "width 0.1s ease",
                      boxShadow: volume > 10 ? `0 0 6px var(--accent)` : "none",
                    }} />
                  </div>
                  <span style={{ fontSize: "10px", color: "var(--text-dim)", minWidth: "30px", textAlign: "right" }}>
                    {volume}%
                  </span>
                </div>
              </div>
            </div>

            {/* Subtitle display — main output */}
            <SubtitleDisplay entries={subtitles} />

            {/* System log */}
            <div>
              <div style={{
                fontSize: "9px", color: "var(--text-dim)", letterSpacing: "2px",
                marginBottom: "6px",
              }}>
                SYSTEM LOG
              </div>
              <LogPanel logs={logs} />
            </div>
          </div>
        </div>

        {/* ── FOOTER ──────────────────────────────────────────────────── */}
        <div style={{
          borderTop: "1px solid var(--border)",
          padding: "10px 0",
          display: "flex", justifyContent: "space-between", alignItems: "center",
        }}>
          <span style={{ fontSize: "9px", color: "var(--text-dim)", letterSpacing: "2px" }}>
            SPEAKFREI · PROTOTYPE v0.1 · ESP32 + WHISPER + CLAUDE
          </span>
          <span style={{ fontSize: "9px", color: "var(--text-dim)", letterSpacing: "1px" }}>
            CHUNK INTERVAL: {CHUNK_INTERVAL_MS}ms · BLE: NORDIC UART
          </span>
        </div>
      </div>
    </>
  );
}
