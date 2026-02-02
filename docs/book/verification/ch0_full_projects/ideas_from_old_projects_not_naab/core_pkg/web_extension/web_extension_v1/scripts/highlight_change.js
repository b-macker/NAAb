// scripts/highlight_change.js
/**
 * Observes table cells and highlights them when their content changes.
 */
(function() {
  const observer = new MutationObserver(mutations => {
    mutations.forEach(m => {
      if (m.type === 'characterData') {
        const parent = m.target.parentElement;
        parent.classList.add('blank-highlight');
        setTimeout(() => parent.classList.remove('blank-highlight'), 800);
      }
    });
  });
  document.querySelectorAll('table td').forEach(td => {
    observer.observe(td.firstChild, { characterData: true });
  });
})();
