// scripts/adaptive_table_handler.js
/**
 * Detects and logs tables inside Shadow DOMs.
 */
(function() {
  function scan(node) {
    if (node.nodeType !== Node.ELEMENT_NODE) return;
    if (node.tagName === 'TABLE') {
      console.log('[Blank] Found table:', node);
    }
    if (node.shadowRoot) {
      scan(node.shadowRoot);
    }
    node.childNodes.forEach(scan);
  }
  document.querySelectorAll('body *').forEach(scan);
})();
