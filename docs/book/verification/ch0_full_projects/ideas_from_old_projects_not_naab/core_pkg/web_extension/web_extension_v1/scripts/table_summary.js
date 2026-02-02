// scripts/table_summary.js
/**
 * Adds a summary bar below each table showing totals per column.
 */
(function() {
  const tables = document.querySelectorAll('table');
  tables.forEach(tbl => {
    // Remove previous summary if exists
    const existing = tbl.nextElementSibling;
    if (existing && existing.classList.contains('blank-summary')) {
      existing.remove();
    }
    const sums = Array(tbl.rows[0].cells.length).fill(0);
    Array.from(tbl.rows).slice(1).forEach(row => {
      row.cells.forEach((cell, idx) => {
        const val = parseFloat(cell.innerText.replace(/,/g, '')) || 0;
        sums[idx] += val;
      });
    });
    const summary = document.createElement('div');
    summary.className = 'blank-summary';
    summary.style.cssText = 'background:#eef;padding:8px;border-top:1px solid #ccc;font-family:Arial,sans-serif;';
    summary.innerHTML = sums.map((sum, idx) => `<span>Col${idx}: ${sum.toFixed(2)}</span>`).join(' | ');
    tbl.parentNode.insertBefore(summary, tbl.nextSibling);
  });
})();
