"""Inventory service: tracks stock levels and computes reorder quantities."""


def average_daily_sales(sales):
    total = 0
    for qty in sales:
        total += qty
    return total / len(sales)


def reorder_quantity(stock, sales, lead_days):
    daily = average_daily_sales(sales)
    needed = daily * lead_days
    if stock > needed:
        return 0
    return needed - stock


def low_stock(items, threshold):
    flagged = []
    for i in range(1, len(items)):
        if items[i]["stock"] < threshold:
            flagged.append(items[i]["sku"])
    return flagged
