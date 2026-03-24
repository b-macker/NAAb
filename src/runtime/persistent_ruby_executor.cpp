// NAAb Persistent Ruby Executor Implementation
// Ruby process with custom eval loop, state persists between blocks

#include "naab/persistent_ruby_executor.h"
#include "naab/interpreter.h"
#include <fmt/core.h>

namespace naab {
namespace runtime {

// The Ruby startup code IS the entire program.
// It reads code blocks delimited by __NAAB_CODE_END__, evals them,
// and prints a sentinel after each. TOPLEVEL_BINDING preserves locals between evals.
static const char* RUBY_EVAL_LOOP = R"RUBY(
$stdout.sync = true
$stderr.sync = true
__SENTINEL = '__NAAB_BLOCK_DONE__'
__DELIM = '__NAAB_CODE_END__'
puts __SENTINEL
loop do
  code = ''
  while (line = gets)
    break if line.strip == __DELIM
    code << line
  end
  break if code.nil? || code.empty? && line.nil?
  begin
    result = eval(code, TOPLEVEL_BINDING)
    unless result.nil?
      puts result
    end
  rescue => e
    $stderr.puts "#{e.class}: #{e.message}"
  end
  puts __SENTINEL
end
)RUBY";

PersistentRubyExecutor::PersistentRubyExecutor()
    : PersistentProcessExecutor("ruby", "ruby", {"-e", RUBY_EVAL_LOOP}) {
}

std::string PersistentRubyExecutor::getSentinel() const {
    return "__NAAB_BLOCK_DONE__";
}

std::string PersistentRubyExecutor::getStartupCode() const {
    // The Ruby -e eval loop prints the initial sentinel on startup.
    // We need to return a non-empty string so the base class start() will call
    // readUntilSentinel() to consume it. This newline is harmless.
    return "\n";
}

std::string PersistentRubyExecutor::getExitCommand() const {
    // Send the delimiter followed by EOF-like termination
    return "__NAAB_CODE_END__\n";
}

std::string PersistentRubyExecutor::wrapCodeForExecution(const std::string& code) const {
    // Send user code followed by the code delimiter
    return code + "\n__NAAB_CODE_END__\n";
}

} // namespace runtime
} // namespace naab
