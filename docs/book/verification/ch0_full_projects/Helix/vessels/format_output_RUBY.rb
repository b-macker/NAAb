v = ARGV[0].to_f
if v == 0.0 && ARGV[0] != "0"
    puts "READY"
else
    res = Math.sqrt(v**2 + 0.5)
    100.times { res = Math.sqrt(res + 0.01) }
    printf("%.15f", res)
end