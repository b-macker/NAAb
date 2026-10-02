"""Report writer: renders an inventory summary to a text file."""


def write_summary(path, rows):
    out = open(path, "w")
    for row in rows:
        out.write(f"{row['sku']}: {row['stock']}\n")
    return len(rows)
