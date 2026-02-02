# Test block for Ruby stdout verification

def test_stdout
  puts "Hello from Ruby stdout!"
  puts "This should be visible if stdout fix works"
  "Function returned successfully"
end

def print_numbers
  puts "Counting: 1, 2, 3, 4, 5"
  (1..5).each do |i|
    puts "Number: #{i}"
  end
  "Done counting"
end

def test_math
  x = 42
  y = 58
  result = x + y
  puts "#{x} + #{y} = #{result}"
  result
end

# If called directly
if __FILE__ == $0
  method = ARGV[0]
  if method
    puts send(method)
  end
end
