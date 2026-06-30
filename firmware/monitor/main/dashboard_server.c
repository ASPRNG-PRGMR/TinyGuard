/*
 * dashboard_server.c — TinyGuard Monitor
 *
 * HTTP server on port 80. Two routes:
 *
 *   GET /        — Full HTML/CSS/JS dashboard, auto-polls every 10s
 *   GET /status  — JSON snapshot consumed by the dashboard and curl
 *
 * ARCHITECTURE
 * ------------
 * The dashboard is a single self-contained HTML page. All CSS and JS are
 * inlined. On load it begins polling /status every 10 seconds, accumulating
 * the last 60 data points client-side for sparkline rendering. No page
 * refresh — the DOM is updated in place via fetch() + JSON.
 *
 * Sparklines are rendered on <canvas> elements using the accumulated
 * history array. No chart library — a 40-line canvas drawing function
 * covers all four metrics. This keeps the page fully self-contained.
 *
 * /status JSON structure:
 *   - Device fields (rssi, uptime, stream_active, viewer_count, reconnects)
 *   - Stats fields (learning, samples, mean/stddev per metric)
 *   - fingerprint object (pds, components, alerts)
 *   - correlation object (per-pair r, ema_r, zscore, alert)
 *   - session object (duration, interval, count, ready)
 *   - alerts array (last 8)
 *
 * BUFFER SIZES
 * ------------
 * html[]  — 512 bytes. The full page is served as a single heap-free
 *            transfer. Almost all content is static JS/CSS/HTML written
 *            once. Dynamic values are injected only into the initial
 *            page shell; subsequent updates go through /status + JS.
 *            The static shell is ~10 KB — served from a const string
 *            literal in flash, not a stack buffer.
 * json[]  — 3072 bytes stack-allocated in the /status handler.
 *
 * PHASE 2 PANELS
 * --------------
 *   Device Health    — status badge, PDS badge, heartbeat age, RSSI
 *   Fingerprint      — PDS score, component breakdown (D/C/S)
 *   Correlation      — per-pair r, EMA r, z-score, alert indicator
 *   Session          — duration EMA, interval EMA, session count
 *   Statistics       — mean/stddev + sparkline per metric
 *   Alert Timeline   — last 8 alerts, categorized by source
 */

#include "dashboard_server.h"
#include "device_state.h"
#include "stats_engine.h"
#include "alert_manager.h"
#include "behavior_profile.h"
#include "correlation_tracker.h"
#include "session_tracker.h"
#include "fingerprint_engine.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>

static const char *TAG = "Dashboard";

/* ── helpers ─────────────────────────────────────────────────────────── */

/* ── HTML page (served from flash) ──────────────────────────────────── */
static const char PAGE_HTML[] =
"<!DOCTYPE html><html lang='en'>"
"<head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>TinyGuard</title>"
"<style>"
"body{font-family:system-ui,monospace;background:#0d1117;color:#c9d1d9;"
     "padding:12px;min-height:100vh}"
"h1{color:#f0f6fc;font-size:1.3em;font-weight:600}"
"h2{color:#8b949e;font-size:.75em;font-weight:400;margin-bottom:12px}"
".grid{display:grid;gap:10px;"
      "grid-template-columns:repeat(auto-fill,minmax(280px,1fr))}"
".grid-wide{grid-column:1/-1}"
".card{background:#161b22;border:1px solid #30363d;border-radius:8px;"
      "padding:14px}"
".card-title{color:#8b949e;font-size:.7em;text-transform:uppercase;"
            "letter-spacing:.08em;margin-bottom:10px;font-weight:600}"
".badge{display:inline-block;padding:3px 9px;border-radius:20px;"
       "font-size:.75em;font-weight:700;color:#fff}"
".stat-row{display:flex;justify-content:space-between;align-items:center;"
          "padding:5px 0;border-bottom:1px solid #21262d}"
".stat-row:last-child{border-bottom:none}"
".stat-label{color:#8b949e;font-size:.8em}"
".stat-val{color:#f0f6fc;font-size:.85em;font-weight:600}"
".stat-val.ok{color:#3fb950}"
".stat-val.warn{color:#d29922}"
".stat-val.crit{color:#f85149}"
".stat-val.dim{color:#484f58}"
".pds-bar-wrap{background:#21262d;border-radius:4px;height:8px;"
              "margin:8px 0 4px;overflow:hidden}"
".pds-bar{height:8px;border-radius:4px;transition:width .4s}"
"table{width:100%;border-collapse:collapse;font-size:.8em}"
"th{color:#8b949e;font-weight:500;text-align:left;padding:4px 6px;"
   "border-bottom:1px solid #21262d}"
"td{padding:4px 6px;color:#c9d1d9;border-bottom:1px solid #21262d}"
"tr:last-child td{border-bottom:none}"
".alert-item{padding:6px 0;border-bottom:1px solid #21262d;font-size:.78em;"
            "display:flex;gap:8px;align-items:flex-start}"
".alert-item:last-child{border-bottom:none}"
".alert-tag{font-size:.65em;padding:2px 5px;border-radius:3px;flex-shrink:0;"
           "font-weight:700;margin-top:1px}"
".alert-src{color:#484f58;font-size:.7em;flex-shrink:0}"
"canvas{display:block;width:100%;margin-top:6px}"
".phase2-gate{color:#484f58;font-size:.8em;font-style:italic;padding:6px 0}"
".two-col{display:grid;grid-template-columns:1fr 1fr;gap:6px}"
"@media(max-width:500px){.two-col{grid-template-columns:1fr}}"
".float-pills{position:fixed;bottom:16px;right:16px;display:flex;gap:8px;"
            "z-index:50}"
".float-pill{display:flex;align-items:center;gap:6px;padding:8px 14px;"
            "border-radius:20px;font-size:.78em;font-weight:700;cursor:pointer;"
            "background:#161b22;border:1.5px solid transparent;"
            "box-shadow:0 2px 10px rgba(0,0,0,.45);"
            "transition:transform .15s,box-shadow .15s;user-select:none}"
".float-pill:hover{transform:translateY(-2px);box-shadow:0 4px 16px rgba(0,0,0,.55)}"
".float-pill.warn{color:#d29922}"
".float-pill.info{color:#58a6ff}"
".float-pill.warn.active{border-color:#d29922;background:#2b2210}"
".float-pill.info.active{border-color:#58a6ff;background:#0f2236}"
".float-pill .fp-count{background:rgba(255,255,255,.12);padding:1px 7px;"
                      "border-radius:10px;font-size:.85em;min-width:1.2em;"
                      "text-align:center}"
".filter-status{font-size:.72em;color:#8b949e}"
".filter-status a{color:#58a6ff;cursor:pointer;text-decoration:none;"
                 "margin-left:6px}"
"</style>"
"</head><body>"

"<div style='display:flex;justify-content:space-between;align-items:flex-end;"
            "margin-bottom:12px;flex-wrap:wrap;gap:6px'>"
  "<div><h1>TinyGuard</h1><h2>IoT Camera Behavior Monitor</h2></div>"
  "<div style='display:flex;gap:8px;align-items:center'>"
    "<span id='status-badge' class='badge' style='background:#484f58'>—</span>"
    "<span id='pds-badge'    class='badge' style='background:#484f58'>PDS —</span>"
  "</div>"
"</div>"

"<div class='grid'>"

/* ── Device card ── */
"<div class='card'>"
  "<div class='card-title'>Device</div>"
  "<div class='stat-row'><span class='stat-label'>Last heartbeat</span>"
    "<span class='stat-val' id='d-age'>—</span></div>"
  "<div class='stat-row'><span class='stat-label'>Camera uptime</span>"
    "<span class='stat-val' id='d-uptime'>—</span></div>"
  "<div class='stat-row'><span class='stat-label'>RSSI</span>"
    "<span class='stat-val' id='d-rssi'>—</span></div>"
  "<div class='stat-row'><span class='stat-label'>Stream</span>"
    "<span class='stat-val' id='d-stream'>—</span></div>"
  "<div class='stat-row'><span class='stat-label'>Viewers</span>"
    "<span class='stat-val' id='d-viewers'>—</span></div>"
  "<div class='stat-row'><span class='stat-label'>Reconnects</span>"
    "<span class='stat-val' id='d-reconnects'>—</span></div>"
  "<div class='stat-row'><span class='stat-label'>Packets received</span>"
    "<span class='stat-val' id='d-packets'>—</span></div>"
"</div>"

/* ── Fingerprint card ── */
"<div class='card'>"
  "<div class='card-title'>Behavioral Fingerprint</div>"
  "<div id='fp-gate' class='phase2-gate'>Waiting for profile to mature&#8230;</div>"
  "<div id='fp-body' style='display:none'>"
    "<div style='display:flex;justify-content:space-between;align-items:center'>"
      "<span style='font-size:2em;font-weight:700;color:#f0f6fc' id='fp-pds'>0</span>"
      "<span style='color:#8b949e;font-size:.8em'>/ 100</span>"
    "</div>"
    "<div class='pds-bar-wrap'><div class='pds-bar' id='fp-bar'></div></div>"
    "<div style='color:#8b949e;font-size:.72em;margin-bottom:8px' id='fp-label'>—</div>"
    "<table>"
      "<tr><th>Component</th><th>Score</th><th>Max z</th><th></th></tr>"
      "<tr><td>Metric Drift (40%)</td>"
          "<td id='fp-d-score'>—</td><td id='fp-d-z'>—</td>"
          "<td id='fp-d-rdy' class='stat-val dim'>—</td></tr>"
      "<tr><td>Correlation Drift (35%)</td>"
          "<td id='fp-c-score'>—</td><td id='fp-c-z'>—</td>"
          "<td id='fp-c-rdy' class='stat-val dim'>—</td></tr>"
      "<tr><td>Session Drift (25%)</td>"
          "<td id='fp-s-score'>—</td><td id='fp-s-z'>—</td>"
          "<td id='fp-s-rdy' class='stat-val dim'>—</td></tr>"
    "</table>"
  "</div>"
"</div>"

/* ── Correlation card ── */
"<div class='card'>"
  "<div class='card-title'>Correlation Tracking</div>"
  "<table>"
    "<tr><th>Pair</th><th>r</th><th>EMA r</th><th>z</th><th>&#9888;</th></tr>"
    "<tr id='cr-0'><td>RSSI&#8596;Reconnect</td>"
        "<td id='cr-0-r' class='dim'>—</td>"
        "<td id='cr-0-e' class='dim'>—</td>"
        "<td id='cr-0-z' class='dim'>—</td>"
        "<td id='cr-0-a'>—</td></tr>"
    "<tr id='cr-1'><td>RSSI&#8596;HB&nbsp;Intv</td>"
        "<td id='cr-1-r' class='dim'>—</td>"
        "<td id='cr-1-e' class='dim'>—</td>"
        "<td id='cr-1-z' class='dim'>—</td>"
        "<td id='cr-1-a'>—</td></tr>"
    "<tr id='cr-2'><td>Stream&#8596;Viewers</td>"
        "<td id='cr-2-r' class='dim'>—</td>"
        "<td id='cr-2-e' class='dim'>—</td>"
        "<td id='cr-2-z' class='dim'>—</td>"
        "<td id='cr-2-a'>—</td></tr>"
  "</table>"
"</div>"

/* ── Session card ── */
"<div class='card'>"
  "<div class='card-title'>Session Behavior</div>"
  "<div id='sess-gate' class='phase2-gate'>Awaiting 5 completed sessions&#8230;</div>"
  "<div id='sess-body'>"
    "<div class='stat-row'><span class='stat-label'>Sessions completed</span>"
      "<span class='stat-val' id='sess-count'>—</span></div>"
    "<div class='stat-row'><span class='stat-label'>Currently streaming</span>"
      "<span class='stat-val' id='sess-active'>—</span></div>"
    "<div class='stat-row'><span class='stat-label'>Starts in window</span>"
      "<span class='stat-val' id='sess-window'>—</span></div>"
    "<div class='stat-row'><span class='stat-label'>Avg duration</span>"
      "<span class='stat-val' id='sess-dur'>—</span></div>"
    "<div class='stat-row'><span class='stat-label'>Avg interval</span>"
      "<span class='stat-val' id='sess-int'>—</span></div>"
  "</div>"
"</div>"

/* ── Statistics + sparklines card (full width) ── */
"<div class='card grid-wide'>"
  "<div class='card-title'>Rolling Statistics "
    "<span id='stats-state' style='color:#484f58;font-weight:400'>(learning)</span>"
  "</div>"
  "<div class='two-col'>"
    "<div>"
      "<div style='color:#8b949e;font-size:.75em;margin-bottom:2px'>RSSI</div>"
      "<div class='stat-val' id='stat-rssi'>— dBm</div>"
      "<canvas id='spark-rssi' height='40'></canvas>"
    "</div>"
    "<div>"
      "<div style='color:#8b949e;font-size:.75em;margin-bottom:2px'>HB Interval</div>"
      "<div class='stat-val' id='stat-hb'>— ms</div>"
      "<canvas id='spark-hb' height='40'></canvas>"
    "</div>"
    "<div>"
      "<div style='color:#8b949e;font-size:.75em;margin-bottom:2px'>RSSI Stability "
        "<span style='font-weight:400;color:#484f58'>(stddev — lower&nbsp;=&nbsp;better)</span>"
      "</div>"
      "<div class='stat-val' id='stat-rssi-sd'>— dBm</div>"
      "<canvas id='spark-rssi-sd' height='40'></canvas>"
    "</div>"
    "<div>"
      "<div style='color:#8b949e;font-size:.75em;margin-bottom:2px'>Profile Divergence Score "
        "<span style='font-weight:400;color:#484f58'>(PDS history)</span>"
      "</div>"
      "<div class='stat-val' id='stat-pds-hist'>—</div>"
      "<canvas id='spark-pds' height='40'></canvas>"
    "</div>"
  "</div>"
"</div>"

/* ── Alert timeline (full width) ── */
"<div class='card grid-wide' id='alert-timeline-card'>"
  "<div style='display:flex;justify-content:space-between;margin-bottom:4px'>"
    "<div class='card-title' style='margin-bottom:0'>Alert Timeline</div>"
    "<div style='color:#8b949e;font-size:.75em'>"
      "<span id='alert-total'>0</span> total</div>"
  "</div>"
  "<div class='filter-status' id='filter-status' style='margin-bottom:8px'>"
    "Showing WARN &amp; CRITICAL"
  "</div>"
  "<div id='alert-list'>"
    "<div style='color:#484f58;font-size:.8em'>No alerts yet</div>"
  "</div>"
"</div>"

"</div>" /* end grid */

/* ── Floating alert-level pills ── */
"<div class='float-pills'>"
  "<div class='float-pill warn' id='pill-warn'>"
    "&#9888; WARN <span class='fp-count' id='pill-warn-count'>0</span>"
  "</div>"
  "<div class='float-pill info' id='pill-info'>"
    "&#8505; INFO <span class='fp-count' id='pill-info-count'>0</span>"
  "</div>"
"</div>"

/* ── JavaScript ── */
"<script>"
/* History ring buffer — 60 points per metric */
"const HIST=60;"
"const hist={rssi:[],hb:[],rssiSd:[],pds:[]};"
"let fetchTs=0;"
"let lastAlerts=[];"
"let alertFilter=null;" /* null=default(WARN+CRIT), 'warn', or 'info' */

/* Sparkline renderer */
"function spark(id,data,color){"
  "const c=document.getElementById(id);"
  "if(!c||data.length<2)return;"
  "const W=c.parentElement.clientWidth||200,H=40;"
  "c.width=W;c.height=H;"
  "const ctx=c.getContext('2d');"
  "ctx.clearRect(0,0,W,H);"
  "const min=Math.min(...data),max=Math.max(...data);"
  "const range=max-min||1;"
  "ctx.beginPath();"
  "ctx.strokeStyle=color;"
  "ctx.lineWidth=1.5;"
  "data.forEach((v,i)=>{"
    "const x=i/(data.length-1)*W;"
    "const y=H-(v-min)/range*(H-4)-2;"
    "i===0?ctx.moveTo(x,y):ctx.lineTo(x,y);"
  "});"
  "ctx.stroke();"
  /* Fill under line */
  "ctx.lineTo(W,H);ctx.lineTo(0,H);ctx.closePath();"
  "ctx.fillStyle=color+'22';ctx.fill();"
"}"

/* Helpers */
"function el(id){return document.getElementById(id);}"
"function setText(id,v){const e=el(id);if(e)e.textContent=v;}"
"function setHtml(id,v){const e=el(id);if(e)e.innerHTML=v;}"
"function setStyle(id,p,v){const e=el(id);if(e)e.style[p]=v;}"
"function setClass(id,c){const e=el(id);if(e)e.className='stat-val '+c;}"

/* PDS colour */
"function pdsColor(p){"
  "if(p<25)return'#3fb950';"
  "if(p<50)return'#d29922';"
  "if(p<75)return'#f85149';"
  "return'#b91c1c';"
"}"
"function pdsLabel(p,ready){"
  "if(!ready)return'LEARNING';"
  "if(p<25)return'NORMAL';"
  "if(p<50)return'ELEVATED';"
  "if(p<75)return'SUSPICIOUS';"
  "return'CRITICAL';"
"}"

/* Main update */
"function updateDashboard(d){"
  /* Device */
  "const age=Date.now()-fetchTs;"
  "setText('d-age',(d.last_rx_age_ms!=null?d.last_rx_age_ms:'—')+' ms');"
  "setText('d-uptime',d.uptime_ms?Math.floor(d.uptime_ms/1000)+' s':'—');"
  "setText('d-rssi',d.rssi!=null?d.rssi+' dBm':'—');"
  "setHtml('d-stream',d.stream_active"
    "?\"<span class='ok'>ACTIVE</span>\":\"<span class='dim'>idle</span>\");"
  "setText('d-viewers',d.viewer_count!=null?d.viewer_count:'—');"
  "setText('d-reconnects',d.reconnects!=null?d.reconnects:'—');"
  "setText('d-packets',d.packet_count!=null?d.packet_count:'—');"

  /* Status badge */
  "const sb=el('status-badge');"
  "if(sb){"
    "const s=d.status!=null?d.status:'OFFLINE';"
    "sb.textContent=s;"
    "sb.style.background=s==='MONITORING'?'#238636':s==='LEARNING'?'#1158ab':'#b91c1c';"
  "}"

  /* Stats */
  "const sl=d.learning;"
  "setText('stats-state',sl?'(learning)':'(active)');"
  "if(d.rssi_mean!=null){"
    "setText('stat-rssi',d.rssi_mean.toFixed(1)+' / '+d.rssi_stddev.toFixed(1)+' dBm');"
    "setText('stat-hb',Math.round(d.hb_interval_mean)+' / '+Math.round(d.hb_interval_stddev)+' ms');"
    "setText('stat-rssi-sd',d.rssi_stddev!=null?d.rssi_stddev.toFixed(2)+' dBm':'—');"
  "}"

  /* History accumulation */
  "if(d.rssi!=null){"
    "hist.rssi.push(d.rssi);if(hist.rssi.length>HIST)hist.rssi.shift();"
    "hist.hb.push(d.hb_interval_mean!=null?d.hb_interval_mean:0);if(hist.hb.length>HIST)hist.hb.shift();"
    "hist.rssiSd.push(d.rssi_stddev!=null?d.rssi_stddev:0);if(hist.rssiSd.length>HIST)hist.rssiSd.shift();"
    "const pdsVal=(d.fingerprint&&d.fingerprint.pds!=null)?d.fingerprint.pds:0;"
    "hist.pds.push(pdsVal);if(hist.pds.length>HIST)hist.pds.shift();"
    "setText('stat-pds-hist',d.fingerprint&&d.fingerprint.ready?pdsVal+' / 100':'(maturing…)');"
    "spark('spark-rssi',hist.rssi,'#58a6ff');"
    "spark('spark-hb',hist.hb,'#3fb950');"
    "spark('spark-rssi-sd',hist.rssiSd,'#f0883e');"
    "spark('spark-pds',hist.pds,'#a371f7');"
  "}"

  /* Fingerprint */
  "const fp=d.fingerprint;"
  "if(fp&&fp.ready){"
    "setStyle('fp-gate','display','none');"
    "setStyle('fp-body','display','block');"
    "setText('fp-pds',fp.pds);"
    "const col=pdsColor(fp.pds);"
    "setStyle('fp-bar','width',fp.pds+'%');"
    "setStyle('fp-bar','background',col);"
    "setText('fp-label',pdsLabel(fp.pds,true));"
    "el('pds-badge').textContent='PDS '+fp.pds+' — '+pdsLabel(fp.pds,true);"
    "el('pds-badge').style.background=col;"
    "const md=fp.metric_drift,cd=fp.correlation_drift,sd=fp.session_drift;"
    "setText('fp-d-score',md.sub_score);setText('fp-d-z',md.max_zscore.toFixed(2));"
    "setText('fp-c-score',cd.sub_score);setText('fp-c-z',cd.max_zscore.toFixed(2));"
    "setText('fp-s-score',sd.sub_score);setText('fp-s-z',sd.max_zscore.toFixed(2));"
    "setHtml('fp-d-rdy',md.ready?\"<span class='ok'>&#10003;</span>\":\"<span class='dim'>—</span>\");"
    "setHtml('fp-c-rdy',cd.ready?\"<span class='ok'>&#10003;</span>\":\"<span class='dim'>—</span>\");"
    "setHtml('fp-s-rdy',sd.ready?\"<span class='ok'>&#10003;</span>\":\"<span class='dim'>—</span>\");"
  "}else{"
    "setStyle('fp-gate','display','');"
    "setStyle('fp-body','display','none');"
  "}"

  /* Correlation */
  "const cr=d.correlation;"
  "if(cr&&cr.pairs){"
    "cr.pairs.forEach((p,i)=>{"
      "if(p.ready){"
        "setText('cr-'+i+'-r',p.r.toFixed(3));"
        "setText('cr-'+i+'-e',p.ema_r.toFixed(3));"
        "setText('cr-'+i+'-z',p.zscore.toFixed(2));"
        "setHtml('cr-'+i+'-a',p.alert"
          "?\"<span class='crit'>&#9888;</span>\":"
          "\"<span class='ok'>&#10003;</span>\");"
      "}else{"
        "setText('cr-'+i+'-r','…');"
        "setText('cr-'+i+'-e','…');"
        "setText('cr-'+i+'-z','…');"
        "setText('cr-'+i+'-a','…');"
      "}"
    "});"
  "}"

  /* Session */
  "const se=d.session;"
  "if(se){"
    "setText('sess-count',se.sessions_completed);"
    "setHtml('sess-active',se.in_session"
      "?\"<span class='ok'>YES</span>\":\"<span class='dim'>no</span>\");"
    "setText('sess-window',se.session_count_in_window);"
    "if(se.ready){"
      "setStyle('sess-gate','display','none');"
      "setText('sess-dur',Math.round(se.ema_duration_ms/1000)+"
        "' s \\u00b1 '+Math.round(se.ema_duration_stddev/1000)+' s');"
      "setText('sess-int',Math.round(se.ema_interval_ms/1000)+"
        "' s \\u00b1 '+Math.round(se.ema_interval_stddev/1000)+' s');"
    "}else{"
      "setStyle('sess-gate','display','');"
      "setText('sess-dur','—');setText('sess-int','—');"
    "}"
  "}"

  /* Alerts */
  "setText('alert-total',d.alert_count!=null?d.alert_count:0);"
  "lastAlerts=d.alerts||[];"
  "setText('pill-warn-count',lastAlerts.filter(a=>a.level===1||a.level===2).length);"
  "setText('pill-info-count',lastAlerts.filter(a=>a.level===0).length);"
  "renderAlerts();"
"}"

/* Renders #alert-list according to the active filter.
 * default (alertFilter=null) and 'warn' both show WARN+CRITICAL only —
 * this is what keeps routine INFO noise (e.g. 'Learning complete') out
 * of the main timeline. 'info' shows INFO entries exclusively.        */
"function renderAlerts(){"
  "const al=el('alert-list');"
  "if(!al)return;"
  "const list=lastAlerts.filter(a=>"
    "alertFilter==='info'?a.level===0:(a.level===1||a.level===2));"
  "al.innerHTML=list.length?list.slice().reverse().map(a=>{"
      "const col=a.level===2?'#f85149':a.level===1?'#d29922':'#58a6ff';"
      "const lvl=a.level===2?'CRIT':a.level===1?'WARN':'INFO';"
      "const src=a.src!=null?a.src:'';"
      "return `<div class='alert-item'>`+"
        "`<span class='alert-tag' style='background:${col}'>${lvl}</span>`+"
        "`<span class='alert-src'>[${src}]</span>`+"
        "`<span>${a.msg}</span>`+"
        "`</div>`;"
    "}).join('')"
    ":\"<div style='color:#484f58;font-size:.8em'>No alerts of this type yet</div>\";"
  "const fs=el('filter-status');"
  "if(fs){"
    "if(alertFilter==='info'){"
      "fs.innerHTML=\"Showing INFO only <a id='filter-clear'>&#10005; clear</a>\";"
    "}else if(alertFilter==='warn'){"
      "fs.innerHTML=\"Showing WARN &amp; CRITICAL <a id='filter-clear'>&#10005; clear</a>\";"
    "}else{"
      "fs.innerHTML='Showing WARN &amp; CRITICAL';"
    "}"
    "const cl=el('filter-clear');"
    "if(cl)cl.onclick=()=>setFilter(null);"
  "}"
  "el('pill-warn').classList.toggle('active',alertFilter!=='info');"
  "el('pill-info').classList.toggle('active',alertFilter==='info');"
"}"

/* Pill click -> set/toggle filter, re-render, scroll timeline into view */
"function setFilter(level){"
  "alertFilter=(alertFilter===level)?null:level;"
  "renderAlerts();"
  "if(alertFilter){"
    "const card=el('alert-timeline-card');"
    "if(card)card.scrollIntoView({behavior:'smooth',block:'start'});"
  "}"
"}"
"el('pill-warn').onclick=()=>setFilter('warn');"
"el('pill-info').onclick=()=>setFilter('info');"

/* Polling */
"async function poll(){"
  "try{"
    "const r=await fetch('/status');"
    "if(!r.ok)return;"
    "fetchTs=Date.now();"
    "const d=await r.json();"
    "updateDashboard(d);"
  "}catch(e){console.warn('poll error',e);}"
"}"
"poll();"
"setInterval(poll,10000);"
"</script>"
"</body></html>";

/* ── HTML handler ────────────────────────────────────────────────────── */

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PAGE_HTML, strlen(PAGE_HTML));
    return ESP_OK;
}

/* ── alert_type_prefix ───────────────────────────────────────────────── */
/* Maps alert_type_t enum to a short source-category string used in the
 * JSON "src" field and rendered as [TAG] in the dashboard alert list.
 * Add cases here as new alert types are defined in alert_manager.h.      */
static const char *alert_type_prefix(int type)
{
    switch (type) {
        case 0:  return "STATS";
        case 1:  return "CORR";
        case 2:  return "SESS";
        case 3:  return "FP";
        case 4:  return "DEV";
        default: return "SYS";
    }
}

/* ── JSON /status handler ────────────────────────────────────────────── */

static esp_err_t handle_status(httpd_req_t *req)
{
    device_state_t         dev  = device_state_get();
    stats_snapshot_t       snap = stats_engine_get_snapshot();
    correlation_snapshot_t cs   = correlation_tracker_get_snapshot();
    session_snapshot_t     se   = session_tracker_get_snapshot();
    fingerprint_snapshot_t fp   = fingerprint_engine_get_snapshot();

    alert_t recent[8];
    int     alert_count = alert_manager_get_recent(recent, 8);

    /* Compute status string */
    const char *status = "OFFLINE";
    if (dev.valid) {
        uint32_t age_ms = esp_log_timestamp() - dev.last_rx_tick;
        if (age_ms < 30000) status = snap.learning ? "LEARNING" : "MONITORING";
        else                status = "TIMEOUT";
    }

    static char json[3072];
    int pos = 0;
    int rem = sizeof(json);

#define J(...) do { int _n=snprintf(json+pos,rem,__VA_ARGS__); \
                    if(_n>0){pos+=_n;rem-=_n;} } while(0)

    J("{");

    /* Device */
    J("\"status\":\"%s\",", status);
    J("\"valid\":%s,",            dev.valid ? "true" : "false");
    J("\"rssi\":%d,",             dev.rssi);
    J("\"uptime_ms\":%" PRIu32 ",", dev.uptime_ms);
    J("\"last_rx_age_ms\":%" PRIu32 ",",
      dev.valid ? esp_log_timestamp() - dev.last_rx_tick : 0);
    J("\"stream_active\":%s,",    dev.stream_active ? "true" : "false");
    J("\"viewer_count\":%d,",     dev.viewer_count);
    J("\"reconnects\":%d,",       dev.reconnects);
    J("\"packet_count\":%" PRIu32 ",", dev.packet_count);

    /* Stats */
    J("\"learning\":%s,",            snap.learning ? "true" : "false");
    J("\"samples\":%" PRIu32 ",",    snap.samples_total);
    J("\"rssi_mean\":%.2f,",         snap.rssi.mean);
    J("\"rssi_stddev\":%.2f,",       snap.rssi.stddev);
    J("\"hb_interval_mean\":%.0f,",  snap.heartbeat_interval_ms.mean);
    J("\"hb_interval_stddev\":%.0f,",snap.heartbeat_interval_ms.stddev);
    J("\"reconnect_rate_mean\":%.3f,",snap.reconnect_rate.mean);
    J("\"reconnect_rate_stddev\":%.3f,",snap.reconnect_rate.stddev);
    J("\"viewer_mean\":%.2f,",       snap.viewer_count.mean);
    J("\"viewer_stddev\":%.2f,",     snap.viewer_count.stddev);

    /* Fingerprint */
    J("\"fingerprint\":{"
      "\"pds\":%u,"
      "\"ready\":%s,"
      "\"alert_elevated\":%s,"
      "\"alert_critical\":%s,"
      "\"metric_drift\":{\"sub_score\":%u,\"max_zscore\":%.2f,\"ready\":%s},"
      "\"correlation_drift\":{\"sub_score\":%u,\"max_zscore\":%.2f,\"ready\":%s},"
      "\"session_drift\":{\"sub_score\":%u,\"max_zscore\":%.2f,\"ready\":%s}"
      "},",
      fp.pds,
      fp.ready          ? "true":"false",
      fp.alert_elevated ? "true":"false",
      fp.alert_critical ? "true":"false",
      fp.metric_drift.sub_score,
      (double)fp.metric_drift.max_zscore,
      fp.metric_drift.ready ? "true":"false",
      fp.correlation_drift.sub_score,
      (double)fp.correlation_drift.max_zscore,
      fp.correlation_drift.ready ? "true":"false",
      fp.session_drift.sub_score,
      (double)fp.session_drift.max_zscore,
      fp.session_drift.ready ? "true":"false");

    /* Correlation */
    J("\"correlation\":{\"ready\":%s,\"pairs\":[",
      cs.ready ? "true":"false");
    for (int i = 0; i < CORR_PAIR_COUNT; i++) {
        const corr_pair_snapshot_t *p = &cs.pairs[i];
        J("{\"id\":%d,\"ready\":%s,\"r\":%.3f,"
          "\"ema_r\":%.3f,\"zscore\":%.2f,\"alert\":%s}%s",
          i,
          p->ready        ? "true":"false",
          (double)p->current_r,
          (double)p->ema_r,
          (double)p->zscore,
          p->alert_active ? "true":"false",
          i < CORR_PAIR_COUNT-1 ? "," : "");
    }
    J("]},");

    /* Session */
    J("\"session\":{"
      "\"ready\":%s,"
      "\"sessions_completed\":%" PRIu32 ","
      "\"in_session\":%s,"
      "\"session_count_in_window\":%u,"
      "\"ema_duration_ms\":%.0f,"
      "\"ema_duration_stddev\":%.0f,"
      "\"ema_interval_ms\":%.0f,"
      "\"ema_interval_stddev\":%.0f"
      "},",
      se.ready ? "true":"false",
      se.sessions_completed,
      se.in_session ? "true":"false",
      se.session_count_in_window,
      (double)se.ema_duration_ms,
      (double)se.ema_duration_stddev,
      (double)se.ema_interval_ms,
      (double)se.ema_interval_stddev);

    /* Alerts */
    J("\"alert_count\":%" PRIu32 ",", alert_manager_count());
    J("\"alerts\":[");
    for (int i = 0; i < alert_count; i++) {
        J("{\"level\":%d,\"type\":%d,\"src\":\"%s\","
          "\"msg\":\"%s\",\"ts\":%" PRIu32 "}%s",
          (int)recent[i].level,
          (int)recent[i].type,
          alert_type_prefix(recent[i].type),
          recent[i].message,
          recent[i].timestamp_ms,
          i < alert_count-1 ? "," : "");
    }
    J("]}");

#undef J

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

/* ── start ───────────────────────────────────────────────────────────── */

void dashboard_server_start(void)
{
    httpd_config_t config  = HTTPD_DEFAULT_CONFIG();
    config.server_port     = 80;
    config.max_open_sockets = 4;
    config.stack_size      = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_uri_t root_uri   = { .uri="/"       , .method=HTTP_GET, .handler=handle_root   };
    httpd_uri_t status_uri = { .uri="/status"  , .method=HTTP_GET, .handler=handle_status };

    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &status_uri);

    /* Print the actual DHCP-assigned IP so the user can open it directly.
     * .local mDNS resolution is unreliable on some host OS configurations
     * (Fedora without Avahi, WSL). The IP is always reachable. */
    esp_netif_ip_info_t ip_info = {};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "Dashboard : http://" IPSTR "/",      IP2STR(&ip_info.ip));
        ESP_LOGI(TAG, "Status    : http://" IPSTR "/status", IP2STR(&ip_info.ip));
    } else {
        ESP_LOGI(TAG, "Dashboard : http://tinyguard-monitor.local/");
        ESP_LOGI(TAG, "Status    : http://tinyguard-monitor.local/status");
    }
    ESP_LOGI(TAG, "mDNS also : http://tinyguard-monitor.local/");
}
