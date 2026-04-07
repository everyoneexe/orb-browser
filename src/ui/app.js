// Orb Browser — Chrome UI Controller

let activeTabId = -1;
let isCurrentTabLoading = false;
const tabs = new Map();
const knownTabIds = new Set();
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

    // Add/update elements
    for (const [id, info] of tabs) {
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
    el.draggable = true;

    // Drag and drop reorder
    el.addEventListener('dragstart', (e) => {
        e.dataTransfer.setData('text/plain', id);
        el.classList.add('dragging');
    });
    el.addEventListener('dragend', () => { el.classList.remove('dragging'); });
    el.addEventListener('dragover', (e) => {
        e.preventDefault();
        const dragging = document.querySelector('.tab-item.dragging');
        if (!dragging || dragging === el) return;
        const rect = el.getBoundingClientRect();
        const mid = rect.top + rect.height / 2;
        if (e.clientY < mid) el.parentNode.insertBefore(dragging, el);
        else el.parentNode.insertBefore(dragging, el.nextSibling);
    });

    // Favicon container
    const favContainer = document.createElement('span');
    favContainer.className = 'tab-fav-container';
    el.appendChild(favContainer);

    const title = document.createElement('span');
    title.className = 'tab-title';
    el.appendChild(title);

    const close = document.createElement('button');
    close.className = 'tab-close';
    close.innerHTML = '&times;';
    close.onclick = (e) => { e.stopPropagation(); closeTabAnimated(id, el); };
    el.appendChild(close);

    el.onclick = () => sendCommand({ cmd: 'switchTab', tabId: id });
    el.addEventListener('mousedown', (e) => {
        if (e.button === 1) { e.preventDefault(); e.stopPropagation(); closeTabAnimated(id, el); }
    });

    if (!knownTabIds.has(id)) {
        knownTabIds.add(id);
        el.style.animation = '';
    } else {
        el.style.animation = 'none';
    }

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
    closingTabs.add(tabId);
    el.classList.add('closing');
    el.addEventListener('animationend', () => { closingTabs.delete(tabId); sendCommand({ cmd: 'closeTab', tabId }); }, { once: true });
    setTimeout(() => { if (closingTabs.has(tabId)) { closingTabs.delete(tabId); sendCommand({ cmd: 'closeTab', tabId }); } }, 300);
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
    onTabCreated(id, url, incognito) {
        knownTabIds.delete(id);
        tabs.set(id, { title: 'Loading...', url: url || '', _incognito: !!incognito });
        scheduleRender();
    },
    onTabClosed(id) {
        tabs.delete(id); knownTabIds.delete(id);
        if (activeTabId === id) activeTabId = tabs.size > 0 ? tabs.keys().next().value : -1;
        scheduleRender();
    },
    onActiveTabChanged(id) {
        const prev = activeTabId; activeTabId = id;
        const info = tabs.get(id);
        if (info && prev !== id) info._justSwitched = true;
        scheduleRender();
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
        if (!/^https?:\/\//i.test(url)) {
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

document.querySelectorAll('.shortcut-item').forEach(item => {
    item.onclick = () => {
        const url = item.dataset.url;
        if (url) sendCommand({ cmd: 'newTab', url });
    };
});

document.addEventListener('keydown', (e) => {
    if (e.ctrlKey && e.key === 'l') { e.preventDefault(); orb.focusAddressBar(); }
    if (e.ctrlKey && e.key === 'f') { e.preventDefault(); showFindBar(); }
    if (e.key === 'Escape' && findVisible) hideFindBar();
    if (e.key === 'Escape' && menuVisible) { menuVisible = false; document.getElementById('menu-popup').style.display = 'none'; }
});

// ─── Init ──────────────────────────────────────────────

loadBookmarks();

// Load saved settings
sendCommand({ cmd: 'settingsGet' }).then(data => {
    if (data && data.searchEngine) {
        searchEngine = data.searchEngine;
        document.querySelectorAll('.search-engine-option').forEach(o => {
            o.classList.toggle('active', o.dataset.engine === searchEngine);
        });
    }
}).catch(() => {});
