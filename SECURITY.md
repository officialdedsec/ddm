# Security

## Check that your copy of DDM is genuine

Download DDM only from the [Releases page](https://github.com/officialdedsec/ddm/releases) of this repository or from dedseec.com.

1. **Digital signature.** Right-click `DDM-Setup-x.y.z.exe` → **Properties → Digital Signatures**. The signer must be
   **DedSec Cyber Security Research Institute**.
2. **SHA-256.** Each release lists the installer's SHA-256 in its notes (and in `update.json`). In PowerShell:
   ```powershell
   Get-FileHash .\DDM-Setup-4.3.3.exe -Algorithm SHA256
   ```
   The value must match exactly.
3. **VirusTotal.** Each release links to its VirusTotal scan.

## How updates stay safe
DDM downloads `update.json` from the latest release and accepts it only with DedSec's digital signature
(`update.json.sig`). The installer must then match the SHA-256 in that file **and** be signed by the same publisher as
the DDM already on your PC — otherwise it is deleted and never run.

## Reporting a vulnerability
Please don't open a public issue. E-mail admin@dedseec.com (or use https://dedseec.com/contact) with "Security" in the subject.
We answer within a few days and credit reporters who wish it.
