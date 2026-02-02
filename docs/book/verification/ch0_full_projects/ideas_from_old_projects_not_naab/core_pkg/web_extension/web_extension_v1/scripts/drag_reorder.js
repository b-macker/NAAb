// scripts/drag_reorder.js
/**
 * Enables drag and drop of table columns by header.
 */
(function() {
  document.querySelectorAll('table').forEach(tbl => {
    let srcIdx = null;
    tbl.querySelectorAll('th').forEach((th, idx) => {
      th.setAttribute('draggable', 'true');
      th.addEventListener('dragstart', e => srcIdx = idx);
      th.addEventListener('dragover', e => e.preventDefault());
      th.addEventListener('drop', e => {
        e.preventDefault();
        const tgtIdx = idx;
        Array.from(tbl.rows).forEach(row => {
          row.insertBefore(row.cells[srcIdx], row.cells[tgtIdx]);
        });
      });
    });
  });
})();
