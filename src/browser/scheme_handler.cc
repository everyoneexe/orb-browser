#include "browser/scheme_handler.h"

#include <fstream>
#include <sstream>

OrbSchemeHandlerFactory::OrbSchemeHandlerFactory(const std::string& ui_dir)
    : ui_dir_(ui_dir) {}

CefRefPtr<CefResourceHandler> OrbSchemeHandlerFactory::Create(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    const CefString& scheme_name,
    CefRefPtr<CefRequest> request) {

    std::string url = request->GetURL().ToString();

    // ─── orb://newtab/ ─────────────────────────────────────
    std::string newtab_prefix = "orb://newtab/";
    if (url.find(newtab_prefix) == 0 || url == "orb://newtab") {
        // Extract engine from query string ?engine=xxx
        std::string engine = "google";
        auto qpos = url.find("?engine=");
        if (qpos != std::string::npos) {
            engine = url.substr(qpos + 8);
            auto amp = engine.find('&');
            if (amp != std::string::npos) engine = engine.substr(0, amp);
        }

        // Map engine to display name and search URL
        std::string engine_name = "Google";
        std::string search_url = "https://www.google.com/search?q=";
        std::string engine_color = "#4285f4";
        if (engine == "ddg") {
            engine_name = "DuckDuckGo";
            search_url = "https://duckduckgo.com/?q=";
            engine_color = "#de5833";
        } else if (engine == "bing") {
            engine_name = "Bing";
            search_url = "https://www.bing.com/search?q=";
            engine_color = "#00809d";
        } else if (engine == "brave") {
            engine_name = "Brave Search";
            search_url = "https://search.brave.com/search?q=";
            engine_color = "#fb542b";
        }

        std::string html = R"HTML(<!DOCTYPE html>
<html><head><meta charset="UTF-8"><style>
*{margin:0;padding:0;box-sizing:border-box}
html,body{height:100%;overflow:hidden}
body{
  background:var(--bg,linear-gradient(135deg,#1a1b26,#292e42));
  color:#c0caf5;
  font-family:-apple-system,BlinkMacSystemFont,'Segoe UI','Inter',sans-serif;
  display:flex;flex-direction:column;align-items:center;
  justify-content:flex-start;
  padding-top:22vh;
  user-select:none;
  transition:background .4s ease;
}

/* Logo + brand */
.logo-wrap{display:flex;align-items:center;gap:14px;margin-bottom:28px}
.logo{
  width:44px;height:44px;
  background:)HTML" + engine_color + R"HTML(;
  border-radius:12px;
  display:flex;align-items:center;justify-content:center;
  font-size:22px;font-weight:700;color:#1a1b26;
  transition:all .5s cubic-bezier(.16,1,.3,1);
}
.logo:hover{}
.brand{font-size:38px;font-weight:300;letter-spacing:-1px;color:#c0caf5}

/* Search box */
.search-box{position:relative;width:min(540px,78vw)}
.search-input{
  width:100%;height:46px;
  padding:0 18px 0 46px;
  background:rgba(41,46,66,.85);
  border:1.5px solid rgba(59,66,97,.7);
  border-radius:24px;
  color:#c0caf5;font-size:14px;font-family:inherit;
  outline:none;
  backdrop-filter:blur(8px);
  transition:all .25s ease;
}
.search-input:focus{
  border-color:)HTML" + engine_color + R"HTML(;
  box-shadow:0 0 0 3px )HTML" + engine_color + R"HTML(25,0 8px 32px rgba(0,0,0,.2);
  background:rgba(41,46,66,.95);
}
.search-input::placeholder{color:#565f89}
.search-icon{
  position:absolute;left:15px;top:50%;transform:translateY(-50%);
  width:18px;height:18px;color:#565f89;pointer-events:none;
}

/* Shortcuts grid */
.shortcuts{
  display:flex;flex-wrap:wrap;justify-content:center;
  gap:16px;margin-top:28px;
  max-width:620px;width:90vw;
}
.sc-item{
  display:flex;flex-direction:column;align-items:center;gap:8px;
  width:72px;cursor:pointer;
  text-decoration:none;
  transition:transform .2s cubic-bezier(.16,1,.3,1);
}
.sc-item:hover{transform:translateY(-3px)}
.sc-item:hover .sc-icon{background:rgba(59,66,97,.8);box-shadow:0 4px 16px rgba(0,0,0,.25)}
.sc-icon{
  width:48px;height:48px;border-radius:50%;
  background:rgba(41,46,66,.8);
  border:1px solid rgba(59,66,97,.5);
  display:flex;align-items:center;justify-content:center;
  overflow:hidden;
  transition:all .2s ease;
}
.sc-icon img{width:24px;height:24px;border-radius:4px;object-fit:contain}
.sc-icon-letter{
  font-size:18px;font-weight:600;color:#9aa5ce;
}
.sc-label{
  font-size:11px;color:#9aa5ce;
  max-width:72px;text-align:center;
  white-space:nowrap;overflow:hidden;text-overflow:ellipsis;
}
.sc-add .sc-icon{border:2px dashed rgba(86,95,137,.6);background:transparent}
.sc-add:hover .sc-icon{border-color:#9aa5ce;background:rgba(41,46,66,.4)}
.sc-add .sc-icon svg{color:#565f89}

/* Add shortcut dialog */
.sc-dialog-overlay{
  position:fixed;inset:0;background:rgba(0,0,0,.45);
  display:flex;align-items:center;justify-content:center;
  z-index:200;opacity:0;pointer-events:none;
  transition:opacity .2s;
}
.sc-dialog-overlay.open{opacity:1;pointer-events:auto}
.sc-dialog{
  width:340px;background:#1e2030;
  border:1px solid #292e42;border-radius:14px;
  padding:24px;box-shadow:0 16px 48px rgba(0,0,0,.5);
  transform:scale(.95);transition:transform .25s cubic-bezier(.16,1,.3,1);
}
.sc-dialog-overlay.open .sc-dialog{transform:scale(1)}
.sc-dialog h3{font-size:15px;font-weight:600;margin-bottom:16px}
.sc-dialog label{font-size:12px;color:#9aa5ce;display:block;margin-bottom:4px}
.sc-dialog input{
  width:100%;height:36px;padding:0 12px;
  background:#292e42;border:1.5px solid #3b4261;border-radius:8px;
  color:#c0caf5;font-size:13px;font-family:inherit;outline:none;
  margin-bottom:12px;transition:border-color .15s;
}
.sc-dialog input:focus{border-color:#7aa2f7}
.sc-dialog-btns{display:flex;justify-content:flex-end;gap:8px;margin-top:4px}
.sc-dialog-btns button{
  padding:7px 16px;border-radius:8px;border:none;
  font-size:13px;font-family:inherit;font-weight:500;cursor:pointer;
  transition:all .12s;
}
.sc-btn-cancel{background:#292e42;color:#9aa5ce}
.sc-btn-cancel:hover{background:#3b4261}
.sc-btn-save{background:#7aa2f7;color:#1a1b26}
.sc-btn-save:hover{background:#89b4fa}

/* Context menu for shortcuts */
.sc-ctx{
  position:fixed;background:#1e2030;border:1px solid #292e42;
  border-radius:8px;padding:4px 0;z-index:300;min-width:140px;
  box-shadow:0 8px 24px rgba(0,0,0,.4);
  animation:menu-pop .12s ease-out;
}
@keyframes menu-pop{from{opacity:0;transform:scale(.95)}to{opacity:1;transform:scale(1)}}
.sc-ctx-item{
  padding:7px 14px;font-size:12px;color:#c0caf5;cursor:pointer;
  transition:background .1s;
}
.sc-ctx-item:hover{background:#292e42}
.sc-ctx-item.danger{color:#f7768e}

/* Customize button — bottom right, minimal */
.customize-btn{
  position:fixed;bottom:20px;right:20px;
  width:36px;height:36px;
  background:rgba(41,46,66,.6);
  border:1px solid rgba(59,66,97,.4);
  border-radius:50%;
  color:#565f89;
  cursor:pointer;
  display:flex;align-items:center;justify-content:center;
  transition:background .2s,color .2s,border-color .2s;
  padding:0;
}
.customize-btn svg{width:16px;height:16px;flex-shrink:0}
.customize-btn:hover{
  background:rgba(41,46,66,.9);
  color:#9aa5ce;
  border-color:rgba(59,66,97,.7);
}
.customize-btn:active{opacity:.7}

/* Overlay */
.overlay{
  position:fixed;inset:0;background:rgba(0,0,0,.35);
  opacity:0;pointer-events:none;
  transition:opacity .3s ease;
  z-index:90;
}
.overlay.open{opacity:1;pointer-events:auto}

/* Panel — slides from right */
.panel{
  position:fixed;top:0;right:0;bottom:0;
  width:320px;
  background:#1a1b26;
  border-left:1px solid #232433;
  z-index:100;
  display:flex;flex-direction:column;
  transform:translateX(100%);
  transition:transform .3s cubic-bezier(.4,0,.2,1);
  overflow-y:auto;
}
.panel.open{transform:translateX(0)}

.panel-header{
  display:flex;align-items:center;justify-content:space-between;
  padding:16px 18px 12px;
  border-bottom:1px solid #232433;
}
.panel-title{font-size:13px;font-weight:600;color:#7a7e96;text-transform:uppercase;letter-spacing:.8px}
.panel-close{
  width:28px;height:28px;border:none;background:transparent;
  color:#565f89;border-radius:6px;cursor:pointer;
  display:flex;align-items:center;justify-content:center;
  transition:color .15s;
}
.panel-close:hover{color:#c0caf5}

/* Section */
.section{padding:14px 18px}
.section-head{
  display:flex;align-items:center;justify-content:space-between;
  margin-bottom:10px;
}
.section-title{font-size:11px;font-weight:600;color:#565f89;text-transform:uppercase;letter-spacing:.8px}
.reset-btn{
  font-size:11px;color:#565f89;background:none;border:none;
  cursor:pointer;font-family:inherit;
  transition:color .15s;
}
.reset-btn:hover{color:#9aa5ce}

/* Category tabs */
.cat-tabs{
  display:flex;gap:0;margin-bottom:12px;
  border-bottom:1px solid #232433;
}
.cat-tab{
  flex:1;padding:8px 0;
  background:none;border:none;border-bottom:2px solid transparent;
  color:#565f89;font-size:11px;font-weight:600;font-family:inherit;
  text-transform:uppercase;letter-spacing:.5px;
  cursor:pointer;text-align:center;
  transition:color .15s,border-color .15s;
}
.cat-tab:hover{color:#9aa5ce}
.cat-tab.active{color:)HTML" + engine_color + R"HTML(;border-bottom-color:)HTML" + engine_color + R"HTML(}

/* Wallpaper grid */
.wallpaper-grid{
  display:grid;grid-template-columns:repeat(3,1fr);gap:8px;
}
.wp-item{
  aspect-ratio:4/3;border-radius:8px;cursor:pointer;
  border:2px solid transparent;
  transition:border-color .15s;
  display:flex;align-items:flex-end;justify-content:center;
  overflow:hidden;position:relative;
  padding-bottom:5px;
}
.wp-item:hover{border-color:rgba(122,162,247,.3)}
.wp-item.active{border-color:)HTML" + engine_color + R"HTML(}
.wp-item .wp-label{
  font-size:9px;color:rgba(255,255,255,.65);
  letter-spacing:.3px;
}

/* Upload slot */
.wp-upload{
  border:1.5px dashed #2a2d3e;background:transparent;
  display:flex;flex-direction:column;align-items:center;justify-content:center;
  gap:3px;color:#3b4261;font-size:10px;cursor:pointer;
  padding-bottom:0;
  transition:border-color .15s,color .15s;
}
.wp-upload:hover{border-color:#565f89;color:#565f89}

/* Toggle */
.toggle-row{
  display:flex;align-items:center;gap:12px;
  padding:14px 18px;
  border-top:1px solid #232433;
}
.toggle{
  width:34px;height:20px;border-radius:10px;
  background:#2a2d3e;position:relative;cursor:pointer;
  transition:background .2s;flex-shrink:0;
}
.toggle.on{background:)HTML" + engine_color + R"HTML(}
.toggle-knob{
  width:16px;height:16px;border-radius:50%;
  background:white;position:absolute;top:2px;left:2px;
  transition:transform .2s cubic-bezier(.4,0,.2,1);
}
.toggle.on .toggle-knob{transform:translateX(14px)}
.toggle-info{display:flex;flex-direction:column}
.toggle-label{font-size:12px;font-weight:500;color:#9aa5ce}
.toggle-desc{font-size:10px;color:#565f89}

</style></head><body>

<div class="logo-wrap">
  <div class="logo">O</div>
  <span class="brand">Orb</span>
</div>

<div class="search-box">
  <svg class="search-icon" viewBox="0 0 20 20" fill="none">
    <circle cx="9" cy="9" r="6" stroke="currentColor" stroke-width="2"/>
    <path d="M13.5 13.5L18 18" stroke="currentColor" stroke-width="2" stroke-linecap="round"/>
  </svg>
  <input class="search-input" placeholder="Search with )HTML" + engine_name + R"HTML( or enter address" autofocus spellcheck="false">
</div>

<!-- Shortcuts grid -->
<div class="shortcuts" id="shortcuts"></div>

<!-- Add shortcut dialog -->
<div class="sc-dialog-overlay" id="scDialogOverlay">
  <div class="sc-dialog">
    <h3 id="scDialogTitle">Add shortcut</h3>
    <label>Name</label>
    <input id="scNameInput" placeholder="e.g. YouTube" spellcheck="false">
    <label>URL</label>
    <input id="scUrlInput" placeholder="https://youtube.com" spellcheck="false">
    <div class="sc-dialog-btns">
      <button class="sc-btn-cancel" id="scCancel">Cancel</button>
      <button class="sc-btn-save" id="scSave">Done</button>
    </div>
  </div>
</div>

<!-- Customize button -->
<button class="customize-btn" id="customizeBtn">
  <svg viewBox="0 0 16 16" fill="none">
    <path d="M11.4 1.6a1.6 1.6 0 0 1 2.3 0l.7.7a1.6 1.6 0 0 1 0 2.3L5.8 13.2l-3.4.8.8-3.4z" stroke="currentColor" stroke-width="1.5" stroke-linejoin="round"/>
  </svg>
</button>

<!-- Overlay -->
<div class="overlay" id="overlay"></div>

<!-- Panel -->
<div class="panel" id="panel">
  <div class="panel-header">
    <span class="panel-title">Appearance</span>
    <button class="panel-close" id="panelClose">
      <svg width="14" height="14" viewBox="0 0 14 14" fill="none">
        <path d="M3 3l8 8M11 3l-8 8" stroke="currentColor" stroke-width="2" stroke-linecap="round"/>
      </svg>
    </button>
  </div>

  <div class="section">
    <div class="section-head">
      <span class="section-title">Background</span>
      <button class="reset-btn" id="resetWp">Reset</button>
    </div>
    <div class="cat-tabs">
      <button class="cat-tab active" data-cat="solid">Solid</button>
      <button class="cat-tab" data-cat="gradient">Gradient</button>
    </div>
    <div class="wallpaper-grid" id="wpGrid">
      <!-- Solid colors -->
      <div class="wp-item wp-solid" data-bg="#1a1b26" data-cat="solid" style="background:#1a1b26">
        <span class="wp-label">Default</span>
      </div>
      <div class="wp-item wp-solid" data-bg="#0f0f0f" data-cat="solid" style="background:#0f0f0f">
        <span class="wp-label">Black</span>
      </div>
      <div class="wp-item wp-solid" data-bg="#16161e" data-cat="solid" style="background:#16161e">
        <span class="wp-label">Night</span>
      </div>
      <div class="wp-item wp-solid" data-bg="#1e1e2e" data-cat="solid" style="background:#1e1e2e">
        <span class="wp-label">Mocha</span>
      </div>
      <div class="wp-item wp-solid" data-bg="#191724" data-cat="solid" style="background:#191724">
        <span class="wp-label">Rose</span>
      </div>
      <div class="wp-item wp-solid" data-bg="#282c34" data-cat="solid" style="background:#282c34">
        <span class="wp-label">Atom</span>
      </div>
      <!-- Gradient colors -->
      <div class="wp-item wp-grad" data-bg="linear-gradient(135deg,#1a1b26,#292e42)" data-cat="gradient" style="background:linear-gradient(135deg,#1a1b26,#292e42)">
        <span class="wp-label">Muted</span>
      </div>
      <div class="wp-item wp-grad" data-bg="linear-gradient(135deg,#0f0c29,#302b63,#24243e)" data-cat="gradient" style="background:linear-gradient(135deg,#0f0c29,#302b63,#24243e)">
        <span class="wp-label">Cosmic</span>
      </div>
      <div class="wp-item wp-grad" data-bg="linear-gradient(135deg,#0c0c1d,#1a1040,#2d1b69)" data-cat="gradient" style="background:linear-gradient(135deg,#0c0c1d,#1a1040,#2d1b69)">
        <span class="wp-label">Nebula</span>
      </div>
      <div class="wp-item wp-grad" data-bg="linear-gradient(135deg,#141e30,#243b55)" data-cat="gradient" style="background:linear-gradient(135deg,#141e30,#243b55)">
        <span class="wp-label">Ocean</span>
      </div>
      <div class="wp-item wp-grad" data-bg="linear-gradient(135deg,#1a1a2e,#16213e,#0f3460)" data-cat="gradient" style="background:linear-gradient(135deg,#1a1a2e,#16213e,#0f3460)">
        <span class="wp-label">Midnight</span>
      </div>
      <div class="wp-item wp-grad" data-bg="linear-gradient(135deg,#232526,#414345)" data-cat="gradient" style="background:linear-gradient(135deg,#232526,#414345)">
        <span class="wp-label">Steel</span>
      </div>
      <div class="wp-item wp-upload" id="uploadWp">
        <svg width="16" height="16" viewBox="0 0 20 20" fill="none">
          <path d="M10 4v12M4 10h12" stroke="currentColor" stroke-width="2" stroke-linecap="round"/>
        </svg>
        <span>Upload</span>
      </div>
    </div>
  </div>

  <div class="toggle-row">
    <div class="toggle" id="shortcutsToggle"><div class="toggle-knob"></div></div>
    <div class="toggle-info">
      <span class="toggle-label">Shortcuts</span>
      <span class="toggle-desc">Show saved sites</span>
    </div>
  </div>
</div>

<input type="file" id="fileInput" accept="image/*" style="display:none">

<script>
// Search
document.querySelector('.search-input').addEventListener('keydown',function(e){
  if(e.key==='Enter'&&this.value.trim()){
    var q=this.value.trim();
    if(/^https?:\/\//i.test(q)||/^[a-zA-Z0-9][\w-]*\.[a-zA-Z]{2,}/.test(q)){
      location.href=q.match(/^https?:\/\//i)?q:'https://'+q;
    }else{
      location.href=')HTML" + search_url + R"HTML('+encodeURIComponent(q);
    }
  }
});

// Panel open/close
var panel=document.getElementById('panel');
var overlay=document.getElementById('overlay');
document.getElementById('customizeBtn').onclick=function(){
  panel.classList.add('open');overlay.classList.add('open');
};
function closePanel(){panel.classList.remove('open');overlay.classList.remove('open')}
document.getElementById('panelClose').onclick=closePanel;
overlay.onclick=closePanel;

// Category tabs
var catTabs=document.querySelectorAll('.cat-tab');
var activeCat='solid';
function showCat(cat){
  activeCat=cat;
  catTabs.forEach(function(t){t.classList.toggle('active',t.dataset.cat===cat)});
  document.querySelectorAll('.wp-item[data-cat]').forEach(function(w){
    w.style.display=w.dataset.cat===cat?'':'none';
  });
  // Upload always visible
  document.getElementById('uploadWp').style.display='';
}
catTabs.forEach(function(t){t.onclick=function(){showCat(t.dataset.cat)}});

// Wallpaper selection
var wpItems=document.querySelectorAll('.wp-item:not(.wp-upload)');
var savedBg=localStorage.getItem('orb-newtab-bg');
if(savedBg){
  document.body.style.background=savedBg;document.body.style.setProperty('--bg',savedBg);
  wpItems.forEach(function(w){w.classList.toggle('active',w.dataset.bg===savedBg)});
  // Switch to correct category tab
  var activeWp=document.querySelector('.wp-item.active');
  if(activeWp&&activeWp.dataset.cat){showCat(activeWp.dataset.cat)}
} else {
  // Default: Muted gradient
  showCat('gradient');
  document.querySelector('.wp-item[data-bg="linear-gradient(135deg,#1a1b26,#292e42)"]').classList.add('active');
}
// Initial category filter
showCat(activeCat);

wpItems.forEach(function(w){
  w.onclick=function(){
    wpItems.forEach(function(x){x.classList.remove('active')});
    w.classList.add('active');
    document.body.style.background=w.dataset.bg;
    localStorage.setItem('orb-newtab-bg',w.dataset.bg);
  };
});

// Reset
document.getElementById('resetWp').onclick=function(){
  wpItems.forEach(function(x){x.classList.remove('active')});
  var muted=document.querySelector('.wp-item[data-bg="linear-gradient(135deg,#1a1b26,#292e42)"]');
  if(muted)muted.classList.add('active');
  document.body.style.background='linear-gradient(135deg,#1a1b26,#292e42)';
  localStorage.removeItem('orb-newtab-bg');
  showCat('gradient');
};

// Upload custom wallpaper
document.getElementById('uploadWp').onclick=function(){document.getElementById('fileInput').click()};
document.getElementById('fileInput').onchange=function(e){
  var file=e.target.files[0];if(!file)return;
  var reader=new FileReader();
  reader.onload=function(ev){
    var bg='url('+ev.target.result+') center/cover no-repeat';
    document.body.style.background=bg;
    localStorage.setItem('orb-newtab-bg',bg);
    wpItems.forEach(function(x){x.classList.remove('active')});
  };
  reader.readAsDataURL(file);
};

// Shortcuts toggle
var stog=document.getElementById('shortcutsToggle');
var sOn=localStorage.getItem('orb-shortcuts')!=='false';
if(sOn)stog.classList.add('on');
stog.onclick=function(){
  stog.classList.toggle('on');
  var show=stog.classList.contains('on');
  localStorage.setItem('orb-shortcuts',show);
  document.getElementById('shortcuts').style.display=show?'flex':'none';
};

// ─── Shortcuts ───────────────────────────────────────
var shortcuts=JSON.parse(localStorage.getItem('orb-newtab-shortcuts')||'[]');
var scGrid=document.getElementById('shortcuts');
var editIdx=-1;

// Default shortcuts if empty
if(shortcuts.length===0){
  shortcuts=[
    {name:'Google',url:'https://www.google.com'},
    {name:'GitHub',url:'https://github.com'},
    {name:'YouTube',url:'https://www.youtube.com'},
  ];
  localStorage.setItem('orb-newtab-shortcuts',JSON.stringify(shortcuts));
}

function getFavicon(url){
  try{return new URL(url).origin+'/favicon.ico';}catch(e){return '';}
}

function getInitial(name,url){
  if(name)return name[0].toUpperCase();
  try{return new URL(url).hostname[0].toUpperCase();}catch(e){return '?';}
}

function renderShortcuts(){
  if(localStorage.getItem('orb-shortcuts')==='false'){
    scGrid.style.display='none';return;
  }
  scGrid.style.display='flex';
  var html='';
  shortcuts.forEach(function(sc,i){
    var fav=getFavicon(sc.url);
    html+='<a class="sc-item" href="'+sc.url.replace(/"/g,'&quot;')+'" data-idx="'+i+'">';
    html+='<div class="sc-icon"><img src="'+fav+'" onerror="this.style.display=\'none\';this.nextElementSibling.style.display=\'flex\'">';
    html+='<span class="sc-icon-letter" style="display:none">'+getInitial(sc.name,sc.url)+'</span></div>';
    html+='<span class="sc-label">'+sc.name+'</span></a>';
  });
  // Add shortcut button
  html+='<div class="sc-item sc-add" id="scAddBtn">';
  html+='<div class="sc-icon"><svg width="20" height="20" viewBox="0 0 20 20" fill="none"><path d="M10 4v12M4 10h12" stroke="currentColor" stroke-width="2" stroke-linecap="round"/></svg></div>';
  html+='<span class="sc-label">Add shortcut</span></div>';
  scGrid.innerHTML=html;

  // Add button click
  document.getElementById('scAddBtn').onclick=function(){editIdx=-1;openScDialog('Add shortcut','','')};

  // Right-click context menu
  scGrid.querySelectorAll('.sc-item[data-idx]').forEach(function(el){
    el.addEventListener('contextmenu',function(e){
      e.preventDefault();
      var idx=parseInt(el.dataset.idx);
      showCtx(e.clientX,e.clientY,idx);
    });
  });
}

// Context menu
var ctxEl=null;
function showCtx(x,y,idx){
  hideCtx();
  var div=document.createElement('div');
  div.className='sc-ctx';
  div.style.left=x+'px';div.style.top=y+'px';
  div.innerHTML='<div class="sc-ctx-item" data-act="edit">Edit shortcut</div><div class="sc-ctx-item danger" data-act="remove">Remove</div>';
  document.body.appendChild(div);
  ctxEl=div;
  // Keep in viewport
  var r=div.getBoundingClientRect();
  if(r.right>window.innerWidth)div.style.left=(x-r.width)+'px';
  if(r.bottom>window.innerHeight)div.style.top=(y-r.height)+'px';

  div.querySelectorAll('.sc-ctx-item').forEach(function(item){
    item.onclick=function(){
      var act=item.dataset.act;
      hideCtx();
      if(act==='edit'){
        editIdx=idx;
        openScDialog('Edit shortcut',shortcuts[idx].name,shortcuts[idx].url);
      }else if(act==='remove'){
        shortcuts.splice(idx,1);
        localStorage.setItem('orb-newtab-shortcuts',JSON.stringify(shortcuts));
        renderShortcuts();
      }
    };
  });
  setTimeout(function(){document.addEventListener('click',hideCtx,{once:true})},10);
}
function hideCtx(){if(ctxEl){ctxEl.remove();ctxEl=null;}}

// Dialog
var dlgOverlay=document.getElementById('scDialogOverlay');
function openScDialog(title,name,url){
  document.getElementById('scDialogTitle').textContent=title;
  document.getElementById('scNameInput').value=name;
  document.getElementById('scUrlInput').value=url;
  dlgOverlay.classList.add('open');
  setTimeout(function(){document.getElementById('scNameInput').focus()},100);
}
function closeScDialog(){dlgOverlay.classList.remove('open');editIdx=-1;}
document.getElementById('scCancel').onclick=closeScDialog;
dlgOverlay.onclick=function(e){if(e.target===dlgOverlay)closeScDialog()};
document.getElementById('scSave').onclick=function(){
  var name=document.getElementById('scNameInput').value.trim();
  var url=document.getElementById('scUrlInput').value.trim();
  if(!url)return;
  if(!/^https?:\/\//i.test(url))url='https://'+url;
  if(!name){try{name=new URL(url).hostname.replace('www.','');}catch(e){name=url;}}
  if(editIdx>=0){
    shortcuts[editIdx]={name:name,url:url};
  }else{
    shortcuts.push({name:name,url:url});
  }
  localStorage.setItem('orb-newtab-shortcuts',JSON.stringify(shortcuts));
  closeScDialog();
  renderShortcuts();
};

renderShortcuts();
</script>
</body></html>)HTML";

        auto* handler = new OrbResourceHandler("");
        handler->SetInlineData(html, "text/html");
        return handler;
    }

    // ─── orb://history/ ─────────────────────────────────────
    if (url.find("orb://history") == 0) {
        std::string html = R"HTML(<!DOCTYPE html>
<html><head><meta charset="UTF-8"><title>History</title><style>
*{margin:0;padding:0;box-sizing:border-box}
html,body{height:100%}
body{
  background:#1a1b26;color:#c0caf5;
  font-family:-apple-system,BlinkMacSystemFont,'Segoe UI','Inter',sans-serif;
  padding:0;overflow-y:auto;
}
.page{max-width:720px;margin:0 auto;padding:40px 24px 60px}
.page-header{display:flex;align-items:center;gap:14px;margin-bottom:28px}
.page-icon{
  width:40px;height:40px;border-radius:10px;
  background:rgba(122,162,247,.12);
  display:flex;align-items:center;justify-content:center;
  color:#7aa2f7;
}
.page-title{font-size:24px;font-weight:600;letter-spacing:-.3px}

.search-wrap{position:relative;margin-bottom:24px}
.search-wrap input{
  width:100%;height:40px;padding:0 14px 0 40px;
  background:#292e42;border:1.5px solid #3b4261;border-radius:10px;
  color:#c0caf5;font-size:13px;font-family:inherit;outline:none;
  transition:border-color .2s;
}
.search-wrap input:focus{border-color:#7aa2f7}
.search-wrap input::placeholder{color:#565f89}
.search-wrap svg{position:absolute;left:12px;top:50%;transform:translateY(-50%);color:#565f89;pointer-events:none}

.toolbar{display:flex;align-items:center;justify-content:space-between;margin-bottom:16px}
.clear-btn{
  padding:6px 14px;border-radius:8px;border:none;
  background:rgba(247,118,142,.1);color:#f7768e;
  font-size:12px;font-weight:500;font-family:inherit;
  cursor:pointer;transition:all .15s;
}
.clear-btn:hover{background:rgba(247,118,142,.2)}
.count{font-size:12px;color:#565f89}

.day-label{
  font-size:11px;font-weight:600;color:#565f89;
  text-transform:uppercase;letter-spacing:.8px;
  padding:12px 0 6px;
}
.item{
  display:flex;align-items:center;gap:12px;
  padding:10px 12px;border-radius:10px;
  cursor:pointer;transition:background .12s;
}
.item:hover{background:#292e42}
.item-icon{
  width:28px;height:28px;border-radius:7px;
  background:#292e42;flex-shrink:0;
  display:flex;align-items:center;justify-content:center;
  overflow:hidden;
}
.item-icon img{width:16px;height:16px;object-fit:contain}
.item-body{flex:1;min-width:0}
.item-title{font-size:13px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.item-url{font-size:11px;color:#565f89;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.item-time{font-size:11px;color:#565f89;flex-shrink:0}
.item-delete{
  width:24px;height:24px;border:none;background:transparent;
  color:#565f89;border-radius:6px;cursor:pointer;
  display:none;align-items:center;justify-content:center;
  transition:all .12s;flex-shrink:0;
}
.item:hover .item-delete{display:flex}
.item-delete:hover{background:rgba(247,118,142,.15);color:#f7768e}

.empty{text-align:center;padding:60px 20px;color:#565f89;font-size:14px}
</style></head><body>
<div class="page">
  <div class="page-header">
    <div class="page-icon">
      <svg width="20" height="20" viewBox="0 0 20 20" fill="none">
        <circle cx="10" cy="10" r="7" stroke="currentColor" stroke-width="1.8"/>
        <path d="M10 6v4.5l3 1.5" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>
      </svg>
    </div>
    <h1 class="page-title">History</h1>
  </div>

  <div class="search-wrap">
    <svg width="16" height="16" viewBox="0 0 20 20" fill="none"><circle cx="9" cy="9" r="6" stroke="currentColor" stroke-width="2"/><path d="M13.5 13.5L18 18" stroke="currentColor" stroke-width="2" stroke-linecap="round"/></svg>
    <input id="searchInput" placeholder="Search history..." autofocus spellcheck="false">
  </div>

  <div class="toolbar">
    <span class="count" id="count"></span>
    <button class="clear-btn" id="clearBtn">Clear all history</button>
  </div>

  <div id="list"></div>
</div>

<script>
var allItems=[];

function getFavicon(url){
  try{var u=new URL(url);return u.origin+'/favicon.ico';}catch(e){return '';}
}

function formatTime(ts){
  var d=new Date(ts*1000);
  return d.toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'});
}

function formatDate(ts){
  var d=new Date(ts*1000);
  var now=new Date();
  var today=new Date(now.getFullYear(),now.getMonth(),now.getDate());
  var day=new Date(d.getFullYear(),d.getMonth(),d.getDate());
  var diff=Math.floor((today-day)/(86400000));
  if(diff===0)return 'Today';
  if(diff===1)return 'Yesterday';
  return d.toLocaleDateString([],{weekday:'long',year:'numeric',month:'long',day:'numeric'});
}

function render(items){
  var list=document.getElementById('list');
  if(items.length===0){
    list.innerHTML='<div class="empty">No history yet</div>';
    document.getElementById('count').textContent='';
    return;
  }
  document.getElementById('count').textContent=items.length+' items';
  // Group by day
  var groups={};
  items.forEach(function(it){
    var d=new Date(it.time*1000);
    var key=d.getFullYear()+'-'+(d.getMonth()+1)+'-'+d.getDate();
    if(!groups[key])groups[key]={label:formatDate(it.time),items:[]};
    groups[key].items.push(it);
  });
  var html='';
  Object.keys(groups).forEach(function(key){
    var g=groups[key];
    html+='<div class="day-label">'+g.label+'</div>';
    g.items.forEach(function(it,i){
      var fav=getFavicon(it.url);
      html+='<div class="item" data-url="'+it.url.replace(/"/g,'&quot;')+'">';
      html+='<div class="item-icon"><img src="'+fav+'" onerror="this.style.display=\'none\'"></div>';
      html+='<div class="item-body"><div class="item-title">'+(it.title||it.url)+'</div>';
      html+='<div class="item-url">'+it.url+'</div></div>';
      html+='<span class="item-time">'+formatTime(it.time)+'</span>';
      html+='<button class="item-delete" data-idx="'+i+'" title="Remove">';
      html+='<svg width="12" height="12" viewBox="0 0 14 14" fill="none"><path d="M3 3l8 8M11 3l-8 8" stroke="currentColor" stroke-width="2" stroke-linecap="round"/></svg>';
      html+='</button></div>';
    });
  });
  list.innerHTML=html;

  // Click to navigate
  list.querySelectorAll('.item').forEach(function(el){
    el.onclick=function(e){
      if(e.target.closest('.item-delete'))return;
      location.href=el.dataset.url;
    };
  });
}

function load(){
  window.cefQuery({
    request:JSON.stringify({cmd:'historyList'}),
    persistent:false,
    onSuccess:function(r){
      try{allItems=JSON.parse(r).reverse();}catch(e){allItems=[];}
      render(allItems);
    },
    onFailure:function(){}
  });
}

document.getElementById('searchInput').oninput=function(){
  var q=this.value.toLowerCase();
  if(!q){render(allItems);return;}
  render(allItems.filter(function(it){
    return (it.title||'').toLowerCase().includes(q)||it.url.toLowerCase().includes(q);
  }));
};

document.getElementById('clearBtn').onclick=function(){
  if(!confirm('Clear all browsing history?'))return;
  window.cefQuery({
    request:JSON.stringify({cmd:'historyClear'}),
    persistent:false,
    onSuccess:function(){allItems=[];render([]);},
    onFailure:function(){}
  });
};

load();
</script>
</body></html>)HTML";

        auto* handler = new OrbResourceHandler("");
        handler->SetInlineData(html, "text/html");
        return handler;
    }

    // ─── orb://downloads/ ───────────────────────────────────
    if (url.find("orb://downloads") == 0) {
        std::string html = R"HTML(<!DOCTYPE html>
<html><head><meta charset="UTF-8"><title>Downloads</title><style>
*{margin:0;padding:0;box-sizing:border-box}
html,body{height:100%}
body{
  background:#1a1b26;color:#c0caf5;
  font-family:-apple-system,BlinkMacSystemFont,'Segoe UI','Inter',sans-serif;
  padding:0;overflow-y:auto;
}
.page{max-width:720px;margin:0 auto;padding:40px 24px 60px}
.page-header{display:flex;align-items:center;gap:14px;margin-bottom:28px}
.page-icon{
  width:40px;height:40px;border-radius:10px;
  background:rgba(115,218,202,.12);
  display:flex;align-items:center;justify-content:center;
  color:#73daca;
}
.page-title{font-size:24px;font-weight:600;letter-spacing:-.3px}

.toolbar{display:flex;align-items:center;justify-content:space-between;margin-bottom:16px}
.count{font-size:12px;color:#565f89}
.open-folder{
  padding:6px 14px;border-radius:8px;border:none;
  background:rgba(122,162,247,.1);color:#7aa2f7;
  font-size:12px;font-weight:500;font-family:inherit;
  cursor:pointer;transition:all .15s;
}
.open-folder:hover{background:rgba(122,162,247,.2)}

.dl-item{
  display:flex;align-items:center;gap:14px;
  padding:12px 14px;border-radius:10px;
  transition:background .12s;margin-bottom:2px;
}
.dl-item:hover{background:#292e42}
.dl-icon{
  width:36px;height:36px;border-radius:8px;
  background:#292e42;flex-shrink:0;
  display:flex;align-items:center;justify-content:center;
  color:#9aa5ce;
}
.dl-body{flex:1;min-width:0}
.dl-name{font-size:13px;font-weight:500;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.dl-info{font-size:11px;color:#565f89;display:flex;gap:8px;margin-top:2px}
.dl-progress{
  width:100%;height:4px;background:#292e42;border-radius:2px;
  margin-top:6px;overflow:hidden;
}
.dl-progress-fill{height:100%;background:#7aa2f7;border-radius:2px;transition:width .3s}
.dl-status{flex-shrink:0;font-size:11px;font-weight:500;padding:3px 8px;border-radius:6px}
.dl-status.done{background:rgba(115,218,202,.12);color:#73daca}
.dl-status.active{background:rgba(122,162,247,.12);color:#7aa2f7}
.dl-status.failed{background:rgba(247,118,142,.12);color:#f7768e}

.empty{text-align:center;padding:60px 20px;color:#565f89;font-size:14px}
.empty svg{margin-bottom:12px;opacity:.4}
</style></head><body>
<div class="page">
  <div class="page-header">
    <div class="page-icon">
      <svg width="20" height="20" viewBox="0 0 20 20" fill="none">
        <path d="M10 3v10M6 9l4 4 4-4" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>
        <path d="M3 14v2a1 1 0 001 1h12a1 1 0 001-1v-2" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"/>
      </svg>
    </div>
    <h1 class="page-title">Downloads</h1>
  </div>

  <div class="toolbar">
    <span class="count" id="count"></span>
  </div>

  <div id="list"></div>
</div>

<script>
var downloads=new Map();

function getFileIcon(name){
  var ext=(name||'').split('.').pop().toLowerCase();
  var icons={
    pdf:'#f7768e',zip:'#bb9af7',tar:'#bb9af7',gz:'#bb9af7',
    png:'#73daca',jpg:'#73daca',jpeg:'#73daca',gif:'#73daca',svg:'#73daca',webp:'#73daca',
    mp4:'#ff9e64',mkv:'#ff9e64',avi:'#ff9e64',mov:'#ff9e64',
    mp3:'#e0af68',wav:'#e0af68',flac:'#e0af68',
    js:'#e0af68',ts:'#7aa2f7',py:'#9ece6a',rs:'#ff9e64',
    doc:'#7aa2f7',docx:'#7aa2f7',xls:'#9ece6a',xlsx:'#9ece6a',
  };
  return icons[ext]||'#9aa5ce';
}

function render(){
  var list=document.getElementById('list');
  var items=Array.from(downloads.values()).reverse();
  if(items.length===0){
    list.innerHTML='<div class="empty"><svg width="48" height="48" viewBox="0 0 20 20" fill="none"><path d="M10 3v10M6 9l4 4 4-4" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/><path d="M3 14v2a1 1 0 001 1h12a1 1 0 001-1v-2" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/></svg><div>No downloads yet</div></div>';
    document.getElementById('count').textContent='';
    return;
  }
  document.getElementById('count').textContent=items.length+' files';
  var html='';
  items.forEach(function(dl){
    var color=getFileIcon(dl.name);
    var statusClass=dl.complete?'done':(dl.percent>=0?'active':'failed');
    var statusText=dl.complete?'Done':(dl.percent>=0?dl.percent+'%':'Failed');
    html+='<div class="dl-item">';
    html+='<div class="dl-icon" style="color:'+color+'">';
    html+='<svg width="18" height="18" viewBox="0 0 20 20" fill="none"><rect x="4" y="2" width="12" height="16" rx="2" stroke="currentColor" stroke-width="1.5"/><path d="M8 7h4M8 10h4M8 13h2" stroke="currentColor" stroke-width="1.3" stroke-linecap="round"/></svg>';
    html+='</div>';
    html+='<div class="dl-body"><div class="dl-name">'+dl.name+'</div>';
    html+='<div class="dl-info"><span>'+formatSize(dl.size)+'</span></div>';
    if(!dl.complete&&dl.percent>=0){
      html+='<div class="dl-progress"><div class="dl-progress-fill" style="width:'+dl.percent+'%"></div></div>';
    }
    html+='</div>';
    html+='<span class="dl-status '+statusClass+'">'+statusText+'</span>';
    html+='</div>';
  });
  list.innerHTML=html;
}

function formatSize(bytes){
  if(!bytes||bytes<=0)return '';
  if(bytes<1024)return bytes+' B';
  if(bytes<1048576)return (bytes/1024).toFixed(1)+' KB';
  if(bytes<1073741824)return (bytes/1048576).toFixed(1)+' MB';
  return (bytes/1073741824).toFixed(2)+' GB';
}

// Listen for download events from sidebar chrome
window.orb={
  onDownloadStart:function(id,name){
    downloads.set(id,{id:id,name:name,percent:0,complete:false,size:0});
    render();
  },
  onDownloadProgress:function(id,pct){
    var dl=downloads.get(id);
    if(dl){dl.percent=pct;render();}
  },
  onDownloadEnd:function(id,success){
    var dl=downloads.get(id);
    if(dl){dl.complete=success;dl.percent=success?100:-1;render();}
  }
};

render();
</script>
</body></html>)HTML";

        auto* handler = new OrbResourceHandler("");
        handler->SetInlineData(html, "text/html");
        return handler;
    }

    // ─── orb://chrome/ ──────────────────────────────────────
    // Parse path from orb://chrome/path
    // URL format: orb://chrome/file.html
    std::string prefix = "orb://chrome/";
    std::string path;
    if (url.find(prefix) == 0) {
        path = url.substr(prefix.length());
    }

    // Remove query string and fragment
    auto qpos = path.find('?');
    if (qpos != std::string::npos) path = path.substr(0, qpos);
    auto fpos = path.find('#');
    if (fpos != std::string::npos) path = path.substr(0, fpos);

    if (path.empty()) path = "index.html";

    std::string file_path = ui_dir_ + "/" + path;
    return new OrbResourceHandler(file_path);
}

OrbResourceHandler::OrbResourceHandler(const std::string& file_path)
    : file_path_(file_path) {
    mime_type_ = GetMimeType(file_path);
}

bool OrbResourceHandler::Open(CefRefPtr<CefRequest> request,
                               bool& handle_request,
                               CefRefPtr<CefCallback> callback) {
    // If inline data was set (e.g. newtab page), use it directly
    if (inline_) {
        offset_ = 0;
        handle_request = true;
        return true;
    }

    std::ifstream file(file_path_, std::ios::binary);
    if (!file.is_open()) {
        handle_request = true;
        data_ = "<html><body><h1>404 - File not found</h1><p>" + file_path_ + "</p></body></html>";
        mime_type_ = "text/html";
        return true;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    data_ = ss.str();
    offset_ = 0;
    handle_request = true;
    return true;
}

void OrbResourceHandler::GetResponseHeaders(CefRefPtr<CefResponse> response,
                                             int64_t& response_length,
                                             CefString& redirect_url) {
    response->SetMimeType(mime_type_);
    response->SetStatus(200);
    response_length = static_cast<int64_t>(data_.size());
}

bool OrbResourceHandler::Read(void* data_out,
                               int bytes_to_read,
                               int& bytes_read,
                               CefRefPtr<CefResourceReadCallback> callback) {
    if (offset_ >= data_.size()) {
        bytes_read = 0;
        return false;
    }

    size_t remaining = data_.size() - offset_;
    size_t to_copy = std::min(static_cast<size_t>(bytes_to_read), remaining);
    memcpy(data_out, data_.data() + offset_, to_copy);
    offset_ += to_copy;
    bytes_read = static_cast<int>(to_copy);
    return true;
}

void OrbResourceHandler::Cancel() {
    data_.clear();
    offset_ = 0;
}

std::string OrbResourceHandler::GetMimeType(const std::string& path) {
    if (path.length() >= 5 && path.substr(path.length() - 5) == ".html") return "text/html";
    if (path.length() >= 4 && path.substr(path.length() - 4) == ".css") return "text/css";
    if (path.length() >= 3 && path.substr(path.length() - 3) == ".js") return "application/javascript";
    if (path.length() >= 4 && path.substr(path.length() - 4) == ".svg") return "image/svg+xml";
    if (path.length() >= 4 && path.substr(path.length() - 4) == ".png") return "image/png";
    if (path.length() >= 4 && path.substr(path.length() - 4) == ".ico") return "image/x-icon";
    if (path.length() >= 5 && path.substr(path.length() - 5) == ".json") return "application/json";
    return "application/octet-stream";
}
