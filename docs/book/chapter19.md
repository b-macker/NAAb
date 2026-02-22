# Chapter 19: Cryptography and Security Modules

Securing data is a fundamental requirement for modern applications. NAAb provides a `crypto` module that offers essential cryptographic primitives, allowing you to hash data, generate secure random numbers, and perform basic encryption.

## 19.1 Hashing with the `crypto` Module

Hashing functions transform data into a fixed-size string of characters, which is typically a digest that represents the data. The `crypto` module supports common algorithms like MD5, SHA-1, SHA-256, and SHA-512.

```naab
use crypto

main {
    let data = "secret_password"

    // SHA-256 Hash
    let hash = crypto.sha256(data)
    print("SHA-256:", hash)

    // SHA-512 Hash
    let hash512 = crypto.sha512(data)
    print("SHA-512:", hash512)
    
    // MD5 (Legacy, not recommended for security)
    print("MD5:", crypto.md5(data))
}
```

## 19.2 Secure Randomness

For generating cryptographic keys, tokens, or salts, you should use a cryptographically secure random number generator (CSPRNG), not the standard math random functions.

```naab
main {
    // Generate random bytes (hex encoded)
    let random_hex = crypto.random_bytes(16)
    print("Random Token:", random_hex)
}
```

## 19.3 Encryption

For encryption and decryption, use a polyglot block with a mature library. The crypto module currently focuses on hashing and randomness:

```naab
main {
    let plaintext = "Sensitive Data"
    let key = "my_secret_key_32bytes_long_12345"

    let encrypted = <<python[plaintext, key]
from cryptography.fernet import Fernet
import base64, hashlib
key_bytes = base64.urlsafe_b64encode(hashlib.sha256(key.encode()).digest())
f = Fernet(key_bytes)
f.encrypt(plaintext.encode()).decode()
>>

    print("Encrypted:", encrypted)
}
```

**Security Note:** Always manage encryption keys securely. Avoid hardcoding keys in your source code. Use environment variables (see Chapter 9) or a dedicated secrets management solution.