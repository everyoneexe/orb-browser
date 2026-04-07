// Orb Browser — Chrome UI Controller

let activeTabId = -1;
let isCurrentTabLoading = false;
const tabs = new Map();
const knownTabIds = new Set();
const pinnedTabIds = new Set();  // track which tab IDs are pinned
const pinnedTabElements = new Map();  // tabId -> DOM element
let bookmarks = [];
let searchEngine = 'google';

const SEARCH_URLS = {
    google: 'https://www.google.com/search?q=',
    ddg: 'https://duckduckgo.com/?q=',
    bing: 'https://www.bing.com/search?q=',
    brave: 'https://search.brave.com/search?q=',
};

const SEARCH_HOME = {
    google: 'https://www.google.com',
    ddg: 'https://duckduckgo.com',
    bing: 'https://www.bing.com',
    brave: 'https://search.brave.com',
};

function getNewTabUrl() {
    return 'orb://newtab/?engine=' + searchEngine;
}

// ─── Commands ───────────────────────────────────────────

function sendCommand(cmd) {
    return new Promise((resolve, reject) => {
        if (typeof window.cefQuery === 'undefined') {
            console.warn('cefQuery not available');
            reject(new Error('cefQuery not available'));
            return;
        }
        window.cefQuery({
            request: JSON.stringify(cmd),
            persistent: false,
            onSuccess: (response) => {
                try { resolve(JSON.parse(response)); }
                catch { resolve(response); }
            },
            onFailure: (code, msg) => reject(new Error(`${code}: ${msg}`)),
        });
    });
}

// ─── Tab rendering ──────────────────────────────────────

const tabElements = new Map();  // tabId → DOM element
let renderScheduled = false;

function scheduleRender() {
    if (!renderScheduled) {
        renderScheduled = true;
        requestAnimationFrame(() => { renderScheduled = false; renderTabs(); });
    }
}

function renderTabs() {
    const list = document.getElementById('tab-list');
    const currentIds = new Set(tabs.keys());

    // Remove elements for closed tabs
    for (const [id, el] of tabElements) {
        if (!currentIds.has(id)) {
            el.remove();
            tabElements.delete(id);
            knownTabIds.delete(id);
        }
    }

    // Add/update elements (skip pinned tabs)
    for (const [id, info] of tabs) {
        if (pinnedTabIds.has(id)) continue;
        let el = tabElements.get(id);
        if (!el) {
            el = createTabElement(id, info);
            tabElements.set(id, el);
            list.appendChild(el);
        }
        updateTabElement(id, info, el);
    }
}

function createTabElement(id, info) {
    const el = document.createElement('div');
    el.className = 'tab-item';
    el.dataset.tabId = id;

    // Favicon container
    const favContainer = document.createElement('span');
    favContainer.className = 'tab-fav-container';
    el.appendChild(favContainer);

    const title = document.createElement('span');
    title.className = 'tab-title';
    el.appendChild(title);

    const close = document.createElement('button');
    close.className = 'tab-close';
    close.innerHTML = '<svg viewBox="0 0 10 10" fill="none"><path d="M2 2l6 6M8 2l-6 6" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/></svg>';
    close.onclick = (e) => { e.stopPropagation(); closeTabAnimated(id, el); };
    el.appendChild(close);

    el.onclick = () => sendCommand({ cmd: 'switchTab', tabId: id });
    el.addEventListener('mousedown', (e) => {
        if (e.button === 1) { e.preventDefault(); e.stopPropagation(); closeTabAnimated(id, el); }
        if (e.button === 0 && !e.target.closest('.tab-close')) startTabDrag(e, el, id);
    });

    if (!knownTabIds.has(id)) {
        knownTabIds.add(id);
        el.style.animation = '';
    } else {
        el.style.animation = 'none';
    }

    // Right-click context menu
    el.addEventListener('contextmenu', (e) => {
        e.preventDefault();
        showTabContextMenu(id, e.clientX, e.clientY);
    });

    return el;
}

function updateTabElement(id, info, el) {
    // Active state
    const isActive = id === activeTabId;
    el.classList.toggle('active', isActive);
    el.classList.toggle('incognito', !!info._incognito);

    // Switching animation
    if (isActive && info._justSwitched) {
        el.classList.add('switching-in');
        info._justSwitched = false;
        el.addEventListener('animationend', () => el.classList.remove('switching-in'), { once: true });
    }

    // Title
    const titleEl = el.querySelector('.tab-title');
    const newTitle = (info._incognito ? '\u{1F576} ' : '') + (info.title || info.url || 'New Tab');
    if (titleEl.textContent !== newTitle) titleEl.textContent = newTitle;

    // Favicon — only update if URL origin changed
    const favContainer = el.querySelector('.tab-fav-container');
    let origin = '';
    try { origin = new URL(info.url).origin; } catch {}
    if (favContainer.dataset.origin !== origin) {
        favContainer.dataset.origin = origin;
        favContainer.innerHTML = '';
        if (info.url && !info.url.startsWith('orb://') && origin) {
            const img = document.createElement('img');
            img.className = 'tab-favicon';
            img.src = origin + '/favicon.ico';
            img.onerror = () => { const ph = document.createElement('div'); ph.className = 'tab-favicon-placeholder'; img.replaceWith(ph); };
            favContainer.appendChild(img);
        } else {
            const ph = document.createElement('div');
            ph.className = 'tab-favicon-placeholder';
            favContainer.appendChild(ph);
        }
    }
}

// ─── Animated tab close ────────────────────────────────

let closingTabs = new Set();
function closeTabAnimated(tabId, el) {
    if (closingTabs.has(tabId)) return;
    if (pinnedTabIds.has(tabId)) return;  // Can't close pinned tabs
    closingTabs.add(tabId);
    el.classList.add('closing');
    el.addEventListener('animationend', () => { closingTabs.delete(tabId); sendCommand({ cmd: 'closeTab', tabId }); }, { once: true });
    setTimeout(() => { if (closingTabs.has(tabId)) { closingTabs.delete(tabId); sendCommand({ cmd: 'closeTab', tabId }); } }, 300);
}

// ─── Essentials (pinned tabs) ─────────────────────────

const MAX_ESSENTIALS = 12;
const essentials = new Map();  // url -> { url, tabId, el }

function renderPinnedTab(tabId, url) {
    const container = document.getElementById('pinned-tabs');

    // If this essential already exists (e.g. loaded via click), update tabId
    const existing = essentials.get(url);
    if (existing) {
        existing.tabId = tabId;
        existing.el.classList.toggle('unloaded', false);
        pinnedTabElements.set(tabId, existing.el);
        updateEssentialsVisibility();
        return;
    }

    const el = document.createElement('div');
    el.className = 'pinned-tab';
    el.dataset.tabId = tabId || '';
    el.dataset.url = url;

    let origin = '';
    try { origin = new URL(url).origin; } catch {}

    if (origin) {
        const img = document.createElement('img');
        img.src = origin + '/favicon.ico';
        img.onerror = () => {
            const letter = document.createElement('span');
            letter.className = 'pinned-letter';
            try { letter.textContent = new URL(url).hostname[0].toUpperCase(); } catch { letter.textContent = '?'; }
            img.replaceWith(letter);
        };
        el.appendChild(img);
    } else {
        const letter = document.createElement('span');
        letter.className = 'pinned-letter';
        letter.textContent = '?';
        el.appendChild(letter);
    }

    // Click: load if unloaded, switch if loaded
    el.onclick = () => {
        const ess = essentials.get(url);
        if (ess && ess.tabId !== null) {
            sendCommand({ cmd: 'switchTab', tabId: ess.tabId });
        } else {
            // Load: create a new tab for this essential
            sendCommand({ cmd: 'newTab', url, pinned: true });
        }
    };

    // Middle-click: unload (close browser) but keep in essentials
    // Middle-click: unload
    el.addEventListener('mousedown', (e) => {
        if (e.button === 1) {
            e.preventDefault();
            e.stopPropagation();
            const ess = essentials.get(url);
            if (ess && ess.tabId !== null) {
                unloadEssential(url);
            }
        }
        if (e.button === 0) startEssentialDrag(e, el, url);
    });

    // Right-click opens context menu
    el.addEventListener('contextmenu', (e) => {
        e.preventDefault();
        const ess = essentials.get(url);
        showEssentialContextMenu(url, ess, e.clientX, e.clientY);
    });

    const essData = { url, tabId: tabId || null, el };
    essentials.set(url, essData);
    if (tabId) {
        pinnedTabElements.set(tabId, el);
        if (tabId === activeTabId) el.classList.add('active');
    } else {
        el.classList.add('unloaded');
    }
    container.appendChild(el);
    updateEssentialsVisibility();
}

function renderEssentialIcon(url) {
    // Render an unloaded essential (no tab yet)
    renderPinnedTab(null, url);
}

function updateEssentialsVisibility() {
    const container = document.getElementById('pinned-tabs');
    const count = essentials.size;
    container.style.display = count > 0 ? 'grid' : 'none';
    // Adaptive columns: 1->1, 2->2, 3->3, 4->4, 5+->3
    let cols;
    if (count <= 4) cols = count;
    else cols = 3;
    container.dataset.cols = cols;
}

function unloadEssential(url) {
    const ess = essentials.get(url);
    if (!ess || ess.tabId === null) return;
    const oldTabId = ess.tabId;

    // If this is the active tab, switch to another first
    if (oldTabId === activeTabId) {
        const nonEssentialTab = [...tabs.keys()].find(id => !pinnedTabIds.has(id));
        if (nonEssentialTab !== undefined) {
            sendCommand({ cmd: 'switchTab', tabId: nonEssentialTab });
        }
    }

    sendCommand({ cmd: 'closeTab', tabId: oldTabId });
    pinnedTabIds.delete(oldTabId);
    pinnedTabElements.delete(oldTabId);
    tabs.delete(oldTabId);
    ess.tabId = null;
    ess.el.classList.add('unloaded');
    ess.el.classList.remove('active');
    ess.el.dataset.tabId = '';
    scheduleRender();
}

function pinTabAsEssential(tabId) {
    if (pinnedTabIds.has(tabId)) return;
    if (essentials.size >= MAX_ESSENTIALS) return;
    const info = tabs.get(tabId);
    if (!info) return;
    sendCommand({ cmd: 'pinTab', tabId, url: info.url }).then(() => {
        pinnedTabIds.add(tabId);
        info._pinned = true;
        renderPinnedTab(tabId, info.url);
        const el = tabElements.get(tabId);
        if (el) { el.remove(); tabElements.delete(tabId); }
    });
}

function unpinEssential(tabId) {
    // Find essential by tabId
    let essUrl = null;
    for (const [url, ess] of essentials) {
        if (ess.tabId === tabId) { essUrl = url; break; }
    }
    if (!essUrl) return;
    const info = tabs.get(tabId);
    if (!info) return;
    sendCommand({ cmd: 'unpinTab', tabId, url: info.url }).then(() => {
        pinnedTabIds.delete(tabId);
        info._pinned = false;
        const ess = essentials.get(essUrl);
        if (ess) { ess.el.remove(); essentials.delete(essUrl); pinnedTabElements.delete(tabId); }
        updateEssentialsVisibility();
        scheduleRender();
    });
}

function removeEssentialByUrl(url) {
    const ess = essentials.get(url);
    if (!ess) return;
    // If loaded, close the tab and unpin
    if (ess.tabId !== null) {
        unpinEssential(ess.tabId);
    } else {
        // Unloaded — just remove from pinned.json and UI
        sendCommand({ cmd: 'unpinTab', tabId: -1, url }).then(() => {
            ess.el.remove();
            essentials.delete(url);
            updateEssentialsVisibility();
        });
    }
}

// ─── Tab context menu ────────────────────────────────

function showTabContextMenu(tabId, x, y) {
    hideTabContextMenu();
    const menu = document.getElementById('tab-context-menu');
    const info = tabs.get(tabId);
    if (!info) return;

    menu.innerHTML = '';
    const isEssential = pinnedTabIds.has(tabId);

    // Add/Remove from Essentials
    if (isEssential) {
        addCtxItem(menu, 'Remove from Essentials', () => unpinEssential(tabId));
    } else {
        const item = document.createElement('div');
        item.className = 'ctx-item';
        item.textContent = 'Add to Essentials';
        const badge = document.createElement('span');
        badge.className = 'ctx-badge';
        badge.textContent = essentials.size + ' / ' + MAX_ESSENTIALS;
        item.appendChild(badge);
        if (essentials.size >= MAX_ESSENTIALS) {
            item.style.opacity = '0.4';
            item.style.pointerEvents = 'none';
        } else {
            item.onclick = () => { hideTabContextMenu(); pinTabAsEssential(tabId); };
        }
        menu.appendChild(item);
    }

    addCtxSeparator(menu);
    addCtxItem(menu, 'Reload Tab', () => sendCommand({ cmd: 'reload', tabId }));
    addCtxItem(menu, 'Mute Tab', () => {});
    addCtxSeparator(menu);
    addCtxItem(menu, 'Duplicate Tab', () => {
        sendCommand({ cmd: 'newTab', url: info.url });
    });
    addCtxSeparator(menu);

    if (!isEssential) {
        addCtxItem(menu, 'Close Tab', () => {
            const el = tabElements.get(tabId);
            if (el) closeTabAnimated(tabId, el);
            else sendCommand({ cmd: 'closeTab', tabId });
        }, true);
    }

    positionContextMenu(menu, x, y);
}

function showEssentialContextMenu(url, ess, x, y) {
    hideTabContextMenu();
    const menu = document.getElementById('tab-context-menu');
    menu.innerHTML = '';

    addCtxItem(menu, 'Remove from Essentials', () => removeEssentialByUrl(url));
    addCtxSeparator(menu);

    if (ess && ess.tabId !== null) {
        addCtxItem(menu, 'Reload', () => sendCommand({ cmd: 'reload', tabId: ess.tabId }));
        addCtxItem(menu, 'Unload', () => unloadEssential(url));
    } else {
        addCtxItem(menu, 'Load', () => sendCommand({ cmd: 'newTab', url, pinned: true }));
    }

    positionContextMenu(menu, x, y);
}

function positionContextMenu(menu, x, y) {
    menu.style.display = 'block';
    const rect = menu.getBoundingClientRect();
    const sidebar = document.getElementById('sidebar');
    const sRect = sidebar.getBoundingClientRect();
    let mx = x, my = y;
    if (mx + rect.width > sRect.right) mx = sRect.right - rect.width - 4;
    if (my + rect.height > sRect.bottom) my = sRect.bottom - rect.height - 4;
    if (mx < sRect.left) mx = sRect.left + 4;
    if (my < sRect.top) my = sRect.top + 4;
    menu.style.left = mx + 'px';
    menu.style.top = my + 'px';
}

function addCtxItem(menu, label, onclick, danger) {
    const item = document.createElement('div');
    item.className = 'ctx-item' + (danger ? ' danger' : '');
    item.textContent = label;
    item.onclick = () => { hideTabContextMenu(); onclick(); };
    menu.appendChild(item);
}

function addCtxSeparator(menu) {
    const sep = document.createElement('div');
    sep.className = 'ctx-separator';
    menu.appendChild(sep);
}

function hideTabContextMenu() {
    document.getElementById('tab-context-menu').style.display = 'none';
}

// Close context menu on click outside or Escape
document.addEventListener('click', (e) => {
    if (!document.getElementById('tab-context-menu').contains(e.target)) {
        hideTabContextMenu();
    }
});
document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') hideTabContextMenu();
}, true);

// ─── Mouse-based tab drag system (CEF OSR compatible) ─

let dragState = null; // { el, tabId, startY, started }
const DRAG_THRESHOLD = 5;

function startTabDrag(e, el, tabId) {
    dragState = { el, tabId, startY: e.clientY, started: false };
}

document.addEventListener('mousemove', (e) => {
    if (!dragState) return;
    const dy = Math.abs(e.clientY - dragState.startY);
    if (!dragState.started && dy < DRAG_THRESHOLD) return;

    if (!dragState.started) {
        dragState.started = true;
        dragState.el.classList.add('dragging');
    }

    // Reorder among tab-items
    const list = document.getElementById('tab-list');
    const siblings = [...list.querySelectorAll('.tab-item:not(.dragging)')];
    for (const sib of siblings) {
        const rect = sib.getBoundingClientRect();
        const mid = rect.top + rect.height / 2;
        if (e.clientY < mid) {
            list.insertBefore(dragState.el, sib);
            break;
        } else if (sib === siblings[siblings.length - 1]) {
            list.appendChild(dragState.el);
        }
    }

    // Check if hovering over essentials area
    const pinned = document.getElementById('pinned-tabs');
    const pRect = pinned.getBoundingClientRect();
    if (e.clientX >= pRect.left && e.clientX <= pRect.right && e.clientY >= pRect.top && e.clientY <= pRect.bottom) {
        pinned.classList.add('drag-over');
    } else {
        pinned.classList.remove('drag-over');
    }
});

document.addEventListener('mouseup', (e) => {
    if (!dragState) return;
    const { el, tabId, started } = dragState;
    dragState = null;

    if (!started) return;
    el.classList.remove('dragging');

    // Check if dropped onto essentials
    const pinned = document.getElementById('pinned-tabs');
    pinned.classList.remove('drag-over');
    const pRect = pinned.getBoundingClientRect();
    if (e.clientX >= pRect.left && e.clientX <= pRect.right && e.clientY >= pRect.top && e.clientY <= pRect.bottom) {
        if (!pinnedTabIds.has(tabId)) {
            pinTabAsEssential(tabId);
        }
    }
});

// ─── Essentials drag reorder (mouse-based) ────────────

let essDragState = null;

function startEssentialDrag(e, el, url) {
    essDragState = { el, url, startX: e.clientX, startY: e.clientY, started: false };
}

document.addEventListener('mousemove', (e) => {
    if (!essDragState) return;
    const dx = Math.abs(e.clientX - essDragState.startX);
    const dy = Math.abs(e.clientY - essDragState.startY);
    if (!essDragState.started && dx + dy < DRAG_THRESHOLD) return;

    if (!essDragState.started) {
        essDragState.started = true;
        essDragState.el.classList.add('dragging');
    }

    const container = document.getElementById('pinned-tabs');
    const siblings = [...container.querySelectorAll('.pinned-tab:not(.dragging)')];
    for (const sib of siblings) {
        const rect = sib.getBoundingClientRect();
        const mid = rect.left + rect.width / 2;
        if (e.clientX < mid) {
            container.insertBefore(essDragState.el, sib);
            return;
        }
    }
    if (siblings.length > 0) container.appendChild(essDragState.el);
});

document.addEventListener('mouseup', (e) => {
    if (!essDragState) return;
    const { el, url, started } = essDragState;
    essDragState = null;
    if (!started) return;
    el.classList.remove('dragging');

    // Check if dropped onto tab list area -> unpin
    const tabList = document.getElementById('tab-list');
    const tRect = tabList.getBoundingClientRect();
    if (e.clientX >= tRect.left && e.clientX <= tRect.right && e.clientY >= tRect.top && e.clientY <= tRect.bottom) {
        removeEssentialByUrl(url);
    }
});

// Load essentials on startup (icons only, no browser tabs created)
function loadPinnedTabs() {
    sendCommand({ cmd: 'getPinnedTabs' }).then(data => {
        const pinned = Array.isArray(data) ? data : (function() { try { return JSON.parse(data); } catch { return []; } })();
        pinned.forEach(p => {
            if (p.url) renderEssentialIcon(p.url);
        });
    }).catch(() => {});
}

// ─── Progress bar ──────────────────────────────────────

function showProgressBar() {
    const c = document.getElementById('progress-bar-container');
    const b = document.getElementById('progress-bar');
    c.classList.add('loading'); b.classList.add('indeterminate'); b.style.width = '';
}
function hideProgressBar() {
    const c = document.getElementById('progress-bar-container');
    const b = document.getElementById('progress-bar');
    b.classList.remove('indeterminate'); b.style.width = '100%';
    setTimeout(() => { c.classList.remove('loading'); b.style.width = '0%'; }, 300);
}

// ─── Find in page ──────────────────────────────────────

let findVisible = false;
function showFindBar() {
    document.getElementById('find-bar').style.display = 'flex';
    findVisible = true;
    const inp = document.getElementById('find-input'); inp.focus(); inp.select();
}
function hideFindBar() {
    document.getElementById('find-bar').style.display = 'none';
    findVisible = false;
    document.getElementById('find-input').value = '';
    document.getElementById('find-count').textContent = '';
    sendCommand({ cmd: 'stopFind' });
}
function doFind(forward) {
    const text = document.getElementById('find-input').value;
    if (text) sendCommand({ cmd: 'find', text, forward: forward ? 'true' : 'false' });
}
let findTimer = null;
document.getElementById('find-input').addEventListener('input', () => {
    clearTimeout(findTimer);
    findTimer = setTimeout(() => doFind(true), 150);
});
document.getElementById('find-input').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') { e.preventDefault(); doFind(!e.shiftKey); }
    if (e.key === 'Escape') { e.preventDefault(); hideFindBar(); }
});
document.getElementById('find-next').onclick = () => doFind(true);
document.getElementById('find-prev').onclick = () => doFind(false);
document.getElementById('find-close').onclick = () => hideFindBar();

// ─── Bookmarks ─────────────────────────────────────────

function loadBookmarks() {
    sendCommand({ cmd: 'bookmarkList' }).then(data => {
        bookmarks = Array.isArray(data) ? data : (function() { try { return JSON.parse(data); } catch { return []; } })();
        renderBookmarks(); updateBookmarkStar();
    });
}
function renderBookmarks() {
    const section = document.getElementById('bookmarks-section');
    const list = document.getElementById('bookmark-list');
    list.innerHTML = '';
    if (bookmarks.length === 0) { section.style.display = 'none'; return; }
    section.style.display = 'block';
    bookmarks.forEach(bm => {
        const el = document.createElement('div'); el.className = 'bookmark-item';
        const fav = document.createElement('img'); fav.className = 'tab-favicon';
        try { fav.src = new URL(bm.url).origin + '/favicon.ico'; } catch { fav.src = ''; }
        fav.onerror = () => { const ph = document.createElement('div'); ph.className = 'tab-favicon-placeholder'; fav.replaceWith(ph); };
        el.appendChild(fav);
        const t = document.createElement('span'); t.className = 'bookmark-title'; t.textContent = bm.title || bm.url; el.appendChild(t);
        const rm = document.createElement('button'); rm.className = 'tab-close'; rm.innerHTML = '&times;';
        rm.onclick = (e) => { e.stopPropagation(); sendCommand({ cmd: 'bookmarkRemove', url: bm.url }).then(() => loadBookmarks()); };
        el.appendChild(rm);
        el.onclick = () => { activeTabId >= 0 ? sendCommand({ cmd: 'navigate', tabId: activeTabId, url: bm.url }) : sendCommand({ cmd: 'newTab', url: bm.url }); };
        list.appendChild(el);
    });
}
function isCurrentPageBookmarked() { const info = tabs.get(activeTabId); return info ? bookmarks.some(b => b.url === info.url) : false; }
function updateBookmarkStar() {
    const btn = document.getElementById('bookmark-btn'), icon = document.getElementById('bookmark-icon');
    if (isCurrentPageBookmarked()) { btn.classList.add('bookmarked'); icon.querySelector('path').setAttribute('fill', 'var(--accent)'); }
    else { btn.classList.remove('bookmarked'); icon.querySelector('path').setAttribute('fill', 'none'); }
}
function toggleBookmark() {
    const info = tabs.get(activeTabId); if (!info) return;
    if (isCurrentPageBookmarked()) sendCommand({ cmd: 'bookmarkRemove', url: info.url }).then(() => loadBookmarks());
    else sendCommand({ cmd: 'bookmarkAdd', url: info.url, title: info.title || info.url }).then(() => loadBookmarks());
}
document.getElementById('bookmark-btn').onclick = () => toggleBookmark();

// ─── Downloads ─────────────────────────────────────────

const downloads = new Map();

function renderDownloads() {
    const section = document.getElementById('downloads-section');
    const list = document.getElementById('download-list');
    list.innerHTML = '';
    if (downloads.size === 0) { section.style.display = 'none'; return; }
    section.style.display = 'block';
    for (const [id, dl] of downloads) {
        const el = document.createElement('div'); el.className = 'download-item';
        const name = document.createElement('span'); name.className = 'download-name'; name.textContent = dl.name; el.appendChild(name);
        if (dl.complete) {
            const badge = document.createElement('span'); badge.className = 'download-badge done'; badge.textContent = 'Done'; el.appendChild(badge);
        } else if (dl.percent >= 0) {
            const bar = document.createElement('div'); bar.className = 'download-bar';
            const fill = document.createElement('div'); fill.className = 'download-fill'; fill.style.width = dl.percent + '%';
            bar.appendChild(fill); el.appendChild(bar);
            const pct = document.createElement('span'); pct.className = 'download-pct'; pct.textContent = dl.percent + '%'; el.appendChild(pct);
        }
        list.appendChild(el);
    }
}

// ─── History ───────────────────────────────────────────

let historyVisible = false;

function toggleHistory() {
    historyVisible = !historyVisible;
    document.getElementById('history-panel').style.display = historyVisible ? 'block' : 'none';
    if (historyVisible) loadHistory();
}

function loadHistory() {
    sendCommand({ cmd: 'historyList' }).then(data => {
        let items = Array.isArray(data) ? data : (function() { try { return JSON.parse(data); } catch { return []; } })();
        items.reverse(); // newest first
        const list = document.getElementById('history-list');
        list.innerHTML = '';
        items.slice(0, 100).forEach(h => {
            const el = document.createElement('div'); el.className = 'history-item';
            const t = document.createElement('span'); t.className = 'bookmark-title'; t.textContent = h.title || h.url; el.appendChild(t);
            el.onclick = () => {
                if (activeTabId >= 0) sendCommand({ cmd: 'navigate', tabId: activeTabId, url: h.url });
                else sendCommand({ cmd: 'newTab', url: h.url });
            };
            list.appendChild(el);
        });
    });
}

document.getElementById('history-close').onclick = () => { historyVisible = false; document.getElementById('history-panel').style.display = 'none'; };
document.getElementById('history-clear').onclick = () => {
    sendCommand({ cmd: 'historyClear' }).then(() => loadHistory());
};

// ─── Menu popup ────────────────────────────────────────

let menuVisible = false;

function toggleMenu() {
    menuVisible = !menuVisible;
    document.getElementById('menu-popup').style.display = menuVisible ? 'block' : 'none';
}

document.getElementById('menu-btn').onclick = () => toggleMenu();

// Close menu when clicking outside
document.addEventListener('click', (e) => {
    if (menuVisible && !document.getElementById('menu-popup').contains(e.target) && e.target.id !== 'menu-btn' && !e.target.closest('#menu-btn')) {
        menuVisible = false;
        document.getElementById('menu-popup').style.display = 'none';
    }
});

document.getElementById('menu-history').onclick = () => { toggleMenu(); sendCommand({ cmd: 'newTab', url: 'orb://history/' }); };
document.getElementById('menu-bookmarks').onclick = () => { toggleMenu(); loadBookmarks(); document.getElementById('bookmarks-section').style.display = 'block'; };
document.getElementById('menu-downloads').onclick = () => { toggleMenu(); sendCommand({ cmd: 'newTab', url: 'orb://downloads/' }); };
document.getElementById('menu-zoom-in').onclick = () => { toggleMenu(); sendCommand({ cmd: 'zoomIn' }); };
document.getElementById('menu-zoom-out').onclick = () => { toggleMenu(); sendCommand({ cmd: 'zoomOut' }); };
document.getElementById('menu-zoom-reset').onclick = () => { toggleMenu(); sendCommand({ cmd: 'zoomReset' }); };
document.getElementById('menu-find').onclick = () => { toggleMenu(); showFindBar(); };
document.getElementById('menu-print').onclick = () => { toggleMenu(); sendCommand({ cmd: 'print' }); };
document.getElementById('menu-fullscreen').onclick = () => { toggleMenu(); sendCommand({ cmd: 'fullscreen' }); };
document.getElementById('menu-incognito').onclick = () => { toggleMenu(); sendCommand({ cmd: 'newIncognitoTab' }); };
document.getElementById('menu-download-dir').onclick = () => { toggleMenu(); sendCommand({ cmd: 'setDownloadDir' }); };

// Search engine selector (custom options)
document.querySelectorAll('.search-engine-option').forEach(opt => {
    opt.onclick = () => {
        searchEngine = opt.dataset.engine;
        document.querySelectorAll('.search-engine-option').forEach(o => o.classList.remove('active'));
        opt.classList.add('active');
        sendCommand({ cmd: 'settingsSave', key: 'searchEngine', value: searchEngine });
    };
});

// ─── Search URL helper ─────────────────────────────────

function getSearchUrl(query) {
    return (SEARCH_URLS[searchEngine] || SEARCH_URLS.google) + encodeURIComponent(query);
}

// ─── SSL indicator ────────────────────────────────────

function updateSslIcon(url) {
    const icon = document.getElementById('ssl-icon');
    if (!url || url.startsWith('about:') || url.startsWith('orb://')) {
        icon.className = 'ssl-neutral';
    } else if (url.startsWith('https://')) {
        icon.className = 'ssl-secure';
    } else {
        icon.className = 'ssl-insecure';
    }
}

// ─── Callbacks from C++ ─────────────────────────────────

const orb = {
    onTabCreated(id, url, incognito, pinned) {
        knownTabIds.delete(id);
        tabs.set(id, { title: 'Loading...', url: url || '', _incognito: !!incognito, _pinned: !!pinned });
        if (pinned) {
            pinnedTabIds.add(id);
            renderPinnedTab(id, url);
        }
        scheduleRender();
    },
    onTabClosed(id) {
        tabs.delete(id); knownTabIds.delete(id);
        // If this was an essential, mark it as unloaded (don't remove icon)
        if (pinnedTabIds.has(id)) {
            pinnedTabIds.delete(id);
            pinnedTabElements.delete(id);
            for (const [url, ess] of essentials) {
                if (ess.tabId === id) {
                    ess.tabId = null;
                    ess.el.classList.add('unloaded');
                    ess.el.classList.remove('active');
                    ess.el.dataset.tabId = '';
                    break;
                }
            }
        }
        if (activeTabId === id) activeTabId = tabs.size > 0 ? tabs.keys().next().value : -1;
        scheduleRender();
    },
    onActiveTabChanged(id) {
        const prev = activeTabId; activeTabId = id;
        const info = tabs.get(id);
        if (info && prev !== id) info._justSwitched = true;
        scheduleRender();
        // Update essential highlights
        for (const [url, ess] of essentials) {
            ess.el.classList.toggle('active', ess.tabId === id);
        }
        if (info) document.getElementById('url-bar').value = info.url || '';
        isCurrentTabLoading = false;
        document.getElementById('stop-icon').style.display = 'none';
        document.getElementById('reload-icon').style.display = '';
        document.getElementById('reload-btn').title = 'Reload';
        document.getElementById('reload-btn').classList.remove('loading');
        updateBookmarkStar();
    },
    onAddressChange(tabId, url) {
        const info = tabs.get(tabId); if (info) info.url = url;
        if (tabId === activeTabId) {
            // Show empty URL bar for newtab page
            document.getElementById('url-bar').value = url.startsWith('orb://newtab') ? '' : url;
            updateBookmarkStar();
            updateSslIcon(url);
            // Restore per-domain zoom
            try {
                const domain = new URL(url).host;
                if (domain && info._lastZoomDomain !== domain) {
                    info._lastZoomDomain = domain;
                    sendCommand({ cmd: 'getZoom', domain }).then(r => {
                        if (r && r.zoom !== undefined && r.zoom !== 0) {
                            // Zoom is set via C++ side already, just for new navigations
                        }
                    }).catch(() => {});
                }
            } catch {}
        }
        // Update only this tab's favicon if origin changed
        const el = tabElements.get(tabId);
        if (el && info) updateTabElement(tabId, info, el);
        // Update pinned tab favicon if origin changed
        const pinnedEl = pinnedTabElements.get(tabId);
        if (pinnedEl && info) {
            let origin = '';
            try { origin = new URL(url).origin; } catch {}
            if (pinnedEl.dataset.origin !== origin) {
                pinnedEl.dataset.origin = origin;
                pinnedEl.innerHTML = '';
                if (origin) {
                    const img = document.createElement('img');
                    img.src = origin + '/favicon.ico';
                    img.onerror = () => {
                        const letter = document.createElement('span');
                        letter.className = 'pinned-letter';
                        try { letter.textContent = new URL(url).hostname[0].toUpperCase(); } catch { letter.textContent = '?'; }
                        img.replaceWith(letter);
                    };
                    pinnedEl.appendChild(img);
                } else {
                    const letter = document.createElement('span');
                    letter.className = 'pinned-letter';
                    letter.textContent = '?';
                    pinnedEl.appendChild(letter);
                }
            }
        }
    },
    onTitleChange(tabId, title) {
        const info = tabs.get(tabId); if (info) info.title = title;
        // Update only this tab's title text
        const el = tabElements.get(tabId);
        if (el) {
            const titleEl = el.querySelector('.tab-title');
            const newTitle = (info._incognito ? '\u{1F576} ' : '') + (title || info.url || 'New Tab');
            if (titleEl.textContent !== newTitle) titleEl.textContent = newTitle;
        }
    },
    onLoadingStateChange(tabId, isLoading, canGoBack, canGoForward) {
        if (tabId === activeTabId) {
            isCurrentTabLoading = isLoading;
            const ri = document.getElementById('reload-icon'), si = document.getElementById('stop-icon'), rb = document.getElementById('reload-btn');
            if (isLoading) { ri.style.display='none'; si.style.display=''; rb.title='Stop'; rb.classList.add('loading'); showProgressBar(); }
            else { si.style.display='none'; ri.style.display=''; rb.title='Reload'; rb.classList.remove('loading'); hideProgressBar(); }
            // Update back/forward button states
            document.getElementById('back-btn').classList.toggle('disabled', !canGoBack);
            document.getElementById('fwd-btn').classList.toggle('disabled', !canGoForward);
        }
    },
    onFindResult(activeMatch, totalCount) {
        const el = document.getElementById('find-count');
        if (totalCount === 0) { el.textContent = 'No matches'; el.style.color = 'var(--close-hover)'; }
        else { el.textContent = activeMatch + '/' + totalCount; el.style.color = 'var(--text-secondary)'; }
    },

    // Download callbacks
    onDownloadStart(id, name) { downloads.set(id, { name, percent: 0, complete: false }); renderDownloads(); },
    onDownloadProgress(id, percent) { const dl = downloads.get(id); if (dl) { dl.percent = percent; renderDownloads(); } },
    onDownloadEnd(id, success) {
        const dl = downloads.get(id); if (dl) { dl.complete = true; dl.percent = 100; renderDownloads(); }
        setTimeout(() => { downloads.delete(id); renderDownloads(); }, 5000);
    },

    showFindBar() { showFindBar(); },
    hideFindBar() { hideFindBar(); },
    toggleBookmark() { toggleBookmark(); },
    toggleHistory() { toggleHistory(); },
    focusAddressBar() { const bar = document.getElementById('url-bar'); bar.focus(); bar.select(); },
    onSidebarPinChanged(pinned) {
        const logo = document.getElementById('logo');
        if (pinned) {
            logo.title = 'Sidebar pinned (Ctrl+S to unpin)';
            logo.style.outline = '';
        } else {
            logo.title = 'Sidebar unpinned (Ctrl+S to pin)';
            logo.style.outline = '2px dashed var(--accent)';
            logo.style.outlineOffset = '2px';
        }
    }
};
window.orb = orb;

// ─── Event listeners ────────────────────────────────────

document.getElementById('new-tab-btn').onclick = () => sendCommand({ cmd: 'newTab', url: getNewTabUrl() });
document.getElementById('back-btn').onclick = () => { if (activeTabId >= 0) sendCommand({ cmd: 'goBack', tabId: activeTabId }); };
document.getElementById('fwd-btn').onclick = () => { if (activeTabId >= 0) sendCommand({ cmd: 'goForward', tabId: activeTabId }); };
document.getElementById('reload-btn').onclick = () => {
    if (activeTabId < 0) return;
    sendCommand({ cmd: isCurrentTabLoading ? 'stop' : 'reload', tabId: activeTabId });
};

document.getElementById('url-bar').addEventListener('keydown', (e) => {
    const suggestions = document.getElementById('url-suggestions');
    if (e.key === 'ArrowDown' && suggestions.style.display !== 'none') {
        e.preventDefault();
        const items = suggestions.querySelectorAll('.suggestion-item');
        const active = suggestions.querySelector('.suggestion-item.active');
        if (active) { active.classList.remove('active'); const next = active.nextElementSibling; if (next) next.classList.add('active'); else items[0]?.classList.add('active'); }
        else if (items[0]) items[0].classList.add('active');
        const cur = suggestions.querySelector('.suggestion-item.active');
        if (cur) document.getElementById('url-bar').value = cur.dataset.url;
        return;
    }
    if (e.key === 'ArrowUp' && suggestions.style.display !== 'none') {
        e.preventDefault();
        const items = suggestions.querySelectorAll('.suggestion-item');
        const active = suggestions.querySelector('.suggestion-item.active');
        if (active) { active.classList.remove('active'); const prev = active.previousElementSibling; if (prev) prev.classList.add('active'); else items[items.length-1]?.classList.add('active'); }
        return;
    }
    if (e.key === 'Enter') {
        suggestions.style.display = 'none';
        let url = e.target.value.trim(); if (!url) return;
        if (!/^https?:\/\//i.test(url) && !/^orb:\/\//i.test(url)) {
            url = /^[a-zA-Z0-9_-]+(\.[a-zA-Z0-9_-]+)+(:\d+)?/.test(url) || /^localhost(:\d+)?/.test(url) ? 'https://' + url : getSearchUrl(url);
        }
        if (activeTabId >= 0) sendCommand({ cmd: 'navigate', tabId: activeTabId, url });
        e.target.blur();
    }
    if (e.key === 'Escape') { suggestions.style.display = 'none'; }
});

// Autocomplete
let acHistory = [];
function loadAcHistory() {
    sendCommand({ cmd: 'historyList' }).then(data => {
        acHistory = Array.isArray(data) ? data : (function() { try { return JSON.parse(data); } catch { return []; } })();
        acHistory.reverse();
    }).catch(() => {});
}
loadAcHistory();
setInterval(loadAcHistory, 30000); // refresh every 30s

document.getElementById('url-bar').addEventListener('input', (e) => {
    const q = e.target.value.trim().toLowerCase();
    const box = document.getElementById('url-suggestions');
    if (q.length < 2) { box.style.display = 'none'; return; }

    // Combine history + bookmarks, deduplicate
    const seen = new Set();
    const results = [];
    for (const item of [...acHistory, ...bookmarks]) {
        const url = item.url || '';
        const title = item.title || '';
        if (seen.has(url)) continue;
        if (url.toLowerCase().includes(q) || title.toLowerCase().includes(q)) {
            seen.add(url);
            results.push(item);
            if (results.length >= 8) break;
        }
    }

    if (results.length === 0) { box.style.display = 'none'; return; }

    box.innerHTML = '';
    results.forEach(r => {
        const el = document.createElement('div');
        el.className = 'suggestion-item';
        el.dataset.url = r.url;
        const t = document.createElement('span'); t.className = 'suggestion-title'; t.textContent = r.title || r.url;
        const u = document.createElement('span'); u.className = 'suggestion-url'; u.textContent = r.url;
        el.appendChild(t); el.appendChild(u);
        el.onclick = () => {
            document.getElementById('url-bar').value = r.url;
            box.style.display = 'none';
            if (activeTabId >= 0) sendCommand({ cmd: 'navigate', tabId: activeTabId, url: r.url });
        };
        box.appendChild(el);
    });
    box.style.display = 'block';
});

document.getElementById('url-bar').addEventListener('blur', () => {
    setTimeout(() => { document.getElementById('url-suggestions').style.display = 'none'; }, 150);
});

document.addEventListener('keydown', (e) => {
    if (e.ctrlKey && e.key === 'l') { e.preventDefault(); orb.focusAddressBar(); }
    if (e.ctrlKey && e.key === 'f') { e.preventDefault(); showFindBar(); }
    if (e.key === 'Escape' && findVisible) hideFindBar();
    if (e.key === 'Escape' && menuVisible) { menuVisible = false; document.getElementById('menu-popup').style.display = 'none'; }
});

// ─── Init ──────────────────────────────────────────────

loadBookmarks();
loadPinnedTabs();

// Load saved settings
sendCommand({ cmd: 'settingsGet' }).then(data => {
    if (data && data.searchEngine) {
        searchEngine = data.searchEngine;
        document.querySelectorAll('.search-engine-option').forEach(o => {
            o.classList.toggle('active', o.dataset.engine === searchEngine);
        });
    }
}).catch(() => {});
