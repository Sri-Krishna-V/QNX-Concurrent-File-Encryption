# Encrypted Files have no integrity check and are decrypted on the client

The client turns a file into an Encrypted File by sending it to the server as Data Blocks, and turns it back by decrypting on its own with the shared key. We added no MAC, no key check value, and no DECRYPT message to the server. The project is about concurrency on QNX, not about building a secure file format, and every one of these would add a second protocol path or a second crypto construction to test and explain. Decrypting and then comparing with `cmp` already proves the round trip in the demo.

What `encclient -d` checks, and what it cannot check:

```mermaid
flowchart TB
    IN["report.pdf.enc"] --> H{"starts with the<br/>QNX-ENC v1 header?"}
    H -- "no" --> R1["rejected:<br/>not an Encrypted File"]
    H -- "yes" --> L{"as long as<br/>the header says?"}
    L -- "no" --> R2["rejected:<br/>damaged, cut short"]
    L -- "yes" --> O{"output file<br/>already exists?"}
    O -- "yes" --> R3["refused:<br/>never overwrites a file"]
    O -- "no" --> D["AES-256-CTR, one Data Block at a time,<br/>each with its IV from the header,<br/>on the client with enc.key"]
    D --> OUT["report2.pdf"]
    D -. "no MAC: a wrong key or a changed byte<br/>gives wrong bytes, not an error" .-> GAP["not detected"]

    classDef data fill:#f1f5f9,stroke:#475569,stroke-width:2px,color:#0f172a
    classDef check fill:#dbeafe,stroke:#2563eb,stroke-width:2px,color:#1e3a8a
    classDef bad fill:#fee2e2,stroke:#dc2626,stroke-width:2px,color:#7f1d1d
    classDef good fill:#dcfce7,stroke:#16a34a,stroke-width:2px,color:#14532d
    class IN,OUT data
    class H,L,O check
    class R1,R2,R3,GAP bad
    class D good
```

## Consequences

- A wrong key or a changed byte is not detected: decryption simply gives wrong bytes. AES-CTR alone gives confidentiality, not integrity. Adding a MAC (for example HMAC-SHA256 or AES-CMAC over the header and the encrypted bytes) is the first thing to do if this were used for real.
- Decryption runs on one client thread, so it is slower than encryption on the Worker Pool and is not part of the benchmark.
- The header still records the sizes and IVs, so a file that is not an Encrypted File, or one that has been cut short, is rejected before anything is written.
