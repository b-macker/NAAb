// scripts/quick_replace.js
/**
 * Replaces all occurrences of 'from' with 'to' in table cells.
 */
window.blankQuickReplace = function(from, to) {
  document.querySelectorAll('table td').forEach(td => {
    if (td.innerText === from) {
      td.innerText = to;
      td.style.background = '#fffa65';
      setTimeout(() => td.style.background = '', 1000);
    }
  });
};
