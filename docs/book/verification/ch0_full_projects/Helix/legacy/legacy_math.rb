# legacy_math.rb
def slow_sum(limit)
  # Inefficient Ruby loop
  total = 0.0
  (1..limit.to_i).each do |i|
    total += Math.sqrt(i * i + 0.01)
  end
  return total
end

if __FILE__ == $0
  puts slow_sum(ARGV[0] || 100)
end
