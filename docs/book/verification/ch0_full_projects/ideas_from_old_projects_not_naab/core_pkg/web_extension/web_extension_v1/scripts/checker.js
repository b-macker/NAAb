// scripts/checker.js
/**
 * Checks version.json in docs and notifies if update available.
 */
(async function() {
  try {
    const url = chrome.runtime.getURL('docs/version.json');
    const response = await fetch(url);
    const data = await response.json();
    if (data.version !== '1.0.0') {
      console.warn('[Blank] Update available:', data.version);
    } else {
      console.log('[Blank] Running latest version.');
    }
  } catch (error) {
    console.error('[Blank] Update check failed:', error);
  }
})();
