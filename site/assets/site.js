/* site.js — the keyboard-first behaviour the device implies:
 *   - a live clock and battery readout in the status bar,
 *   - arrow/j/k scrolling, [d]/[h] navigation, [+/-] text size, [?] key help,
 *   - the launcher demo on the landing page, driven like the real one.
 * No dependencies, no network calls.
 */
(function () {
  'use strict';

  var TEXT_SIZES = [16, 24, 32]; // integer-ish multiples of the 16 px design size
  var STORE_KEY = 'eg.textSize';

  // --- status bar ---------------------------------------------------------

  function paintClock() {
    var now = new Date();
    var text = String(now.getHours()).padStart(2, '0') + ':' + String(now.getMinutes()).padStart(2, '0');
    document.querySelectorAll('[data-clock]').forEach(function (el) { el.textContent = text; });
  }

  function startClock() {
    paintClock();
    // Tick on the minute boundary, like the firmware's 1 s status timer does.
    setTimeout(function () {
      paintClock();
      setInterval(paintClock, 60000);
    }, (60 - new Date().getSeconds()) * 1000);
  }

  function paintBattery() {
    var slots = document.querySelectorAll('[data-batt]');
    if (!slots.length) return;
    if (!navigator.getBattery) {
      slots.forEach(function (el) { el.textContent = 'usb'; });
      return;
    }
    navigator.getBattery().then(function (batt) {
      var render = function () {
        var text = batt.charging ? 'chg' : Math.round(batt.level * 100) + '%';
        slots.forEach(function (el) { el.textContent = text; });
      };
      batt.addEventListener('levelchange', render);
      batt.addEventListener('chargingchange', render);
      render();
    }).catch(function () {
      slots.forEach(function (el) { el.textContent = 'usb'; });
    });
  }

  // --- text size ----------------------------------------------------------

  function applyTextSize(px) {
    document.documentElement.style.setProperty('--text', px + 'px');
    try { localStorage.setItem(STORE_KEY, String(px)); } catch (e) { /* private mode */ }
  }

  function stepTextSize(delta) {
    var current = parseInt(getComputedStyle(document.documentElement).getPropertyValue('--text'), 10) || 16;
    var index = TEXT_SIZES.indexOf(current);
    if (index === -1) index = 0;
    applyTextSize(TEXT_SIZES[Math.min(TEXT_SIZES.length - 1, Math.max(0, index + delta))]);
  }

  function restoreTextSize() {
    var saved;
    try { saved = parseInt(localStorage.getItem(STORE_KEY), 10); } catch (e) { saved = NaN; }
    if (TEXT_SIZES.indexOf(saved) !== -1) applyTextSize(saved);
  }

  // --- launcher demo ------------------------------------------------------

  function initDevice() {
    var device = document.getElementById('device');
    if (!device) return;

    var tiles = Array.prototype.slice.call(device.querySelectorAll('.tile'));
    if (!tiles.length) return;

    var grid = document.getElementById('tile-grid');
    var view = document.getElementById('app-view');
    var viewTitle = document.getElementById('app-view-title');
    var viewText = document.getElementById('app-view-text');
    var focused = 0;
    var open = false;

    var blurbs = {};
    document.querySelectorAll('.app-blurb').forEach(function (node) {
      try { blurbs[node.dataset.app] = JSON.parse(node.textContent); } catch (e) { /* ignore */ }
    });

    function paintFocus() {
      tiles.forEach(function (tile, i) { tile.classList.toggle('is-focused', i === focused && !open); });
    }

    /** Tiles per row, read back from the flex layout rather than assumed. */
    function rowLength() {
      var top = tiles[0].offsetTop;
      var n = 0;
      while (n < tiles.length && tiles[n].offsetTop === top) n++;
      return n || 1;
    }

    function move(delta) {
      focused = Math.min(tiles.length - 1, Math.max(0, focused + delta));
      paintFocus();
    }

    function openApp(index) {
      var name = tiles[index].dataset.app;
      open = true;
      viewTitle.textContent = name;
      viewText.textContent = blurbs[name] || '';
      view.hidden = false;
      grid.hidden = true;
      paintFocus();
    }

    function closeApp() {
      open = false;
      view.hidden = true;
      grid.hidden = false;
      paintFocus();
    }

    device.addEventListener('focus', function () { device.classList.add('is-active'); });
    device.addEventListener('blur', function () { device.classList.remove('is-active'); });

    tiles.forEach(function (tile, i) {
      tile.addEventListener('click', function () {
        device.focus();
        focused = i;
        openApp(i);
      });
      tile.addEventListener('mouseenter', function () {
        if (!open) { focused = i; paintFocus(); }
      });
    });

    device.addEventListener('keydown', function (ev) {
      if (ev.metaKey || ev.ctrlKey || ev.altKey) return;
      var key = ev.key;
      if (open) {
        if (key === 'Escape' || key === 'Backspace') { ev.preventDefault(); closeApp(); }
        return;
      }
      switch (key) {
        case 'ArrowLeft': ev.preventDefault(); move(-1); break;
        case 'ArrowRight': ev.preventDefault(); move(1); break;
        case 'ArrowUp': ev.preventDefault(); move(-rowLength()); break;
        case 'ArrowDown': ev.preventDefault(); move(rowLength()); break;
        case 'Enter': ev.preventDefault(); openApp(focused); break;
        case 'Escape': device.blur(); break;
        default: break;
      }
    });

    paintFocus();
  }

  // --- table of contents highlight ----------------------------------------

  function initToc() {
    var items = Array.prototype.slice.call(document.querySelectorAll('.toc-item'));
    if (!items.length || !('IntersectionObserver' in window)) return;

    var byId = {};
    items.forEach(function (item) { byId[item.getAttribute('href').slice(1)] = item; });

    var visible = {};
    var observer = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) { visible[entry.target.id] = entry.isIntersecting; });
      var current = null;
      Object.keys(byId).forEach(function (id) {
        if (visible[id] && !current) current = id;
      });
      items.forEach(function (item) {
        item.classList.toggle('is-current', item.getAttribute('href') === '#' + current);
      });
    }, { rootMargin: '-40px 0px -70% 0px' });

    Object.keys(byId).forEach(function (id) {
      var heading = document.getElementById(id);
      if (heading) observer.observe(heading);
    });
  }

  // --- global keys --------------------------------------------------------

  function initKeys() {
    var overlay = document.getElementById('keys-overlay');
    var root = document.documentElement;
    var prefix = document.body.dataset.page.indexOf('/') === -1 ? '' : '../';

    function toggleOverlay(show) {
      if (!overlay) return;
      overlay.hidden = show === undefined ? !overlay.hidden : !show;
    }

    document.addEventListener('keydown', function (ev) {
      if (ev.metaKey || ev.ctrlKey || ev.altKey) return;
      var target = ev.target;
      if (target && (target.tagName === 'INPUT' || target.tagName === 'TEXTAREA' || target.isContentEditable)) return;
      if (target && target.closest && target.closest('.device')) return; // the panel owns its keys

      var step = parseInt(getComputedStyle(root).getPropertyValue('--text'), 10) * 1.5 || 24;

      switch (ev.key) {
        // Arrow keys are left to the browser; j/k are the vi-ish equivalent.
        case 'j': ev.preventDefault(); window.scrollBy(0, step); break;
        case 'k': ev.preventDefault(); window.scrollBy(0, -step); break;
        case 'g': ev.preventDefault(); window.scrollTo(0, 0); break;
        case 'G': ev.preventDefault(); window.scrollTo(0, document.body.scrollHeight); break;
        case 'd': ev.preventDefault(); window.location.href = prefix + 'docs/'; break;
        case 'h': ev.preventDefault(); window.location.href = prefix || './'; break;
        case '+': case '=': ev.preventDefault(); stepTextSize(1); break;
        case '-': case '_': ev.preventDefault(); stepTextSize(-1); break;
        case '?': ev.preventDefault(); toggleOverlay(); break;
        case 'Escape': toggleOverlay(false); break;
        default: break;
      }
    });

    if (overlay) {
      overlay.addEventListener('click', function () { toggleOverlay(false); });
    }

    document.querySelectorAll('[data-action]').forEach(function (el) {
      el.addEventListener('click', function (ev) {
        ev.preventDefault();
        var action = el.dataset.action;
        if (action === 'text-up') stepTextSize(1);
        else if (action === 'text-down') stepTextSize(-1);
        else if (action === 'keys') toggleOverlay();
      });
    });
  }

  restoreTextSize();
  startClock();
  paintBattery();
  initDevice();
  initToc();
  initKeys();
})();
