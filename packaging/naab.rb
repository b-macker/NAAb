class Naab < Formula
  desc "NAAb Block Assembly Language — polyglot governance-aware scripting"
  homepage "https://github.com/b-macker/NAAb"
  url "https://github.com/b-macker/NAAb/archive/refs/tags/v0.9.0.tar.gz"
  # sha256 "UPDATE_WITH_ACTUAL_SHA256"
  license "MIT"
  head "https://github.com/b-macker/NAAb.git", branch: "master"

  depends_on "cmake" => :build
  depends_on "ninja" => :build
  depends_on "pkg-config" => :build
  depends_on "sqlite"
  depends_on "openssl@3"
  depends_on "curl"
  depends_on "python@3" => :optional

  def install
    args = %W[
      -DCMAKE_BUILD_TYPE=Release
      -DOPENSSL_ROOT_DIR=#{Formula["openssl@3"].opt_prefix}
    ]

    system "cmake", "-S", ".", "-B", "build", "-G", "Ninja", *args, *std_cmake_args
    system "ninja", "-C", "build", "naab-lang", "naab-gov"
    bin.install "build/naab-lang"
    bin.install "build/naab-gov"
  end

  test do
    (testpath/"hello.naab").write <<~NAAB
      main {
        print("Hello from NAAb!")
      }
    NAAB
    assert_match "Hello from NAAb!", shell_output("#{bin}/naab-lang #{testpath}/hello.naab 2>&1")
  end
end
