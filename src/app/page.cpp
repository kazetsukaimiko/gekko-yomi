#include "page.hpp"

namespace gekko::yomi {

std::string page_html() {
    return R"page(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Gekko Yomi</title>
<style>
  :root {
    --ink: #14110e;
    --ink-2: #221c17;
    --paper: #f3ead7;
    --paper-edge: #e4d5b8;
    --cinnabar: #c4452d;
    --moss: #2f4638;
    --faded: #8d7d68;
    --mincho: "IPAMincho", "IPAPMincho", "HanaMinA", "Sazanami Mincho", serif;
    --gothic: "IPAGothic", "IPAPGothic", "Sazanami Gothic", sans-serif;
  }
  * { box-sizing: border-box; }
  html, body { margin: 0; height: 100%; }
  body {
    background:
      radial-gradient(1200px 700px at 10% -10%, #2a221b 0%, transparent 55%),
      var(--ink);
    color: var(--paper);
    font-family: var(--gothic);
    min-height: 100%;
  }
  header {
    display: flex;
    align-items: baseline;
    gap: 1rem;
    padding: 1.1rem 1.4rem 0.4rem;
  }
  header h1 {
    font-family: var(--mincho);
    font-weight: 400;
    font-size: 1.7rem;
    letter-spacing: 0.18em;
    margin: 0;
  }
  header h1 span { color: var(--cinnabar); letter-spacing: 0; margin-right: 0.35rem; }
  header p { margin: 0; color: var(--faded); font-size: 0.85rem; }
  main {
    display: grid;
    grid-template-columns: minmax(0, 1.4fr) minmax(280px, 0.8fr);
    gap: 1rem;
    padding: 0.8rem 1.2rem 1.4rem;
    height: calc(100% - 4.2rem);
  }
  .stage, .sheet { min-height: 70vh; }
  .stage {
    background: var(--ink-2);
    border: 1px solid #3a3128;
    padding: 0.8rem;
    display: flex;
    flex-direction: column;
    gap: 0.7rem;
  }
  .controls { display: flex; gap: 0.5rem; }
  select, button {
    font-family: var(--gothic);
    font-size: 0.95rem;
    background: #1a1612;
    color: var(--paper);
    border: 1px solid #4a3e32;
    padding: 0.45rem 0.7rem;
  }
  select { flex: 1; }
  button { cursor: pointer; background: var(--cinnabar); border-color: var(--cinnabar); color: #fff8f2; }
  button.quiet { background: transparent; color: var(--paper); border-color: #4a3e32; }
  .frame {
    position: relative;
    flex: 1;
    display: grid;
    place-items: center;
    background:
      linear-gradient(180deg, rgba(196,69,45,0.08), transparent 40%),
      #0e0c0a;
    overflow: auto;
  }
  .shot-wrap { position: relative; display: inline-block; max-width: 100%; }
  img#shot { display: block; max-width: 100%; height: auto; }
  #boxes { position: absolute; inset: 0; pointer-events: none; }
  .box { position: absolute; border: 1px solid var(--cinnabar); box-shadow: 0 0 0 1px rgba(20,17,14,0.35); }
  .sheet {
    background: var(--paper);
    color: #241c16;
    padding: 1.2rem 1.2rem 1.4rem 2.4rem;
    position: relative;
    box-shadow: 0 18px 50px rgba(0,0,0,0.28);
  }
  .seal {
    position: absolute;
    left: 0.45rem;
    top: 1rem;
    writing-mode: vertical-rl;
    font-family: var(--mincho);
    letter-spacing: 0.25em;
    color: var(--cinnabar);
    font-size: 1.05rem;
  }
  .status { color: var(--moss); min-height: 1.2rem; font-size: 0.85rem; }
  .run { border-top: 1px solid var(--paper-edge); padding: 0.8rem 0; }
  .run .line { font-family: var(--mincho); font-size: 1.65rem; line-height: 1.7; }
  ruby rt { font-size: 0.45em; color: var(--moss); }
  .gloss { margin: 0.25rem 0 0; color: #5c5144; font-size: 0.92rem; }
  .empty { color: #7a6d5c; font-family: var(--mincho); font-size: 1.2rem; margin-top: 2rem; }
  @media (max-width: 860px) {
    main { grid-template-columns: 1fr; height: auto; }
    .stage, .sheet { min-height: 0; }
  }
</style>
</head>
<body>
<header>
  <h1><span>月読</span>Gekko</h1>
  <p>A window, then the line.</p>
</header>
<main>
  <section class="stage">
    <div class="controls">
      <select id="window"></select>
      <button type="button" id="refresh" class="quiet">Refresh</button>
      <button type="button" id="read">Read</button>
    </div>
    <div class="frame">
      <div class="shot-wrap">
        <img id="shot" alt="">
        <div id="boxes"></div>
      </div>
    </div>
  </section>
  <section class="sheet">
    <div class="seal">月読</div>
    <div class="status" id="status"></div>
    <div id="runs"><p class="empty">Pick a window. The text lands here.</p></div>
  </section>
</main>
<script>
const windowSelect = document.getElementById("window");
const statusLine = document.getElementById("status");
const runsEl = document.getElementById("runs");
const shot = document.getElementById("shot");
const boxes = document.getElementById("boxes");
let last = null;

function setStatus(text) { statusLine.textContent = text || ""; }

async function loadWindows() {
  const response = await fetch("/api/windows");
  const data = await response.json();
  windowSelect.textContent = "";
  const errors = (data.load_errors || []).join(" ");
  if (data.capture_error) {
    setStatus(data.capture_error + (errors ? " " + errors : ""));
  } else if (errors) {
    setStatus(errors);
  } else {
    setStatus(data.windows.length ? "" : "No windows to read.");
  }
  for (const windowInfo of data.windows || []) {
    const option = document.createElement("option");
    option.value = windowInfo.id;
    option.textContent = windowInfo.title + (windowInfo.minimized ? " (minimized)" : "");
    windowSelect.appendChild(option);
  }
}

function renderRuns(data) {
  runsEl.textContent = "";
  if (!data.runs || data.runs.length === 0) {
    const empty = document.createElement("p");
    empty.className = "empty";
    empty.textContent = data.ocr_error || "No text in this grab.";
    runsEl.appendChild(empty);
    return;
  }
  for (const run of data.runs) {
    const block = document.createElement("article");
    block.className = "run";
    const line = document.createElement("div");
    line.className = "line";
    if (run.morphemes && run.morphemes.length) {
      for (const morpheme of run.morphemes) {
        if (morpheme.reading) {
          const ruby = document.createElement("ruby");
          ruby.appendChild(document.createTextNode(morpheme.surface));
          const rt = document.createElement("rt");
          rt.textContent = morpheme.reading;
          ruby.appendChild(rt);
          line.appendChild(ruby);
        } else {
          line.appendChild(document.createTextNode(morpheme.surface));
        }
        if (morpheme.glosses && morpheme.glosses.length) {
          const gloss = document.createElement("p");
          gloss.className = "gloss";
          gloss.textContent = morpheme.surface + " — " + morpheme.glosses.join("; ");
          block.appendChild(gloss);
        }
      }
    } else {
      line.textContent = run.text;
    }
    block.insertBefore(line, block.firstChild);
    runsEl.appendChild(block);
  }
}

function placeBoxes() {
  boxes.textContent = "";
  if (!last || !last.image_width || !shot.clientWidth) return;
  const scale = shot.clientWidth / last.image_width;
  boxes.style.width = shot.clientWidth + "px";
  boxes.style.height = shot.clientHeight + "px";
  for (const run of last.runs || []) {
    const chars = run.characters && run.characters.length ? run.characters : [{box: run.box}];
    for (const character of chars) {
      if (!character.box) continue;
      const mark = document.createElement("div");
      mark.className = "box";
      mark.style.left = (character.box[0] * scale) + "px";
      mark.style.top = (character.box[1] * scale) + "px";
      mark.style.width = (character.box[2] * scale) + "px";
      mark.style.height = (character.box[3] * scale) + "px";
      boxes.appendChild(mark);
    }
  }
}

async function readSelected() {
  const windowId = windowSelect.value;
  if (!windowId) {
    setStatus("No window selected.");
    return;
  }
  setStatus("Reading…");
  const response = await fetch("/api/read", {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    body: JSON.stringify({window_id: windowId})
  });
  const data = await response.json();
  last = data;
  if (data.capture_error) {
    setStatus(data.capture_error);
    runsEl.textContent = "";
    return;
  }
  const notes = [];
  if (data.minimized) notes.push("Minimized. This grab may be stale.");
  if (data.ocr_error) notes.push(data.ocr_error);
  if (data.engine) notes.push(data.engine);
  setStatus(notes.join(" "));
  renderRuns(data);
  shot.onload = placeBoxes;
  shot.src = "/api/frame.bmp?t=" + Date.now();
  if (shot.complete) placeBoxes();
}

document.getElementById("refresh").addEventListener("click", loadWindows);
document.getElementById("read").addEventListener("click", readSelected);
window.addEventListener("resize", placeBoxes);
loadWindows();
</script>
</body>
</html>
)page";
}

}  // namespace gekko::yomi
