export class SemanticExtractor {
  static tableToJSON(table: HTMLTableElement) {
    const rows = Array.from(table.rows).map(r =>
      Array.from(r.cells).map(c => c.innerText.trim()));
    return { header: rows[0] || [], rows: rows.slice(1) };
  }
}
