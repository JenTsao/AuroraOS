import sys
import struct
import zlib
import argparse

FIRMWARE_MAGIC = 0x41555241
VALID_STATUS = 0x12345678
PT_MAGIC = 0x50415254

# Ed25519 signature size in bytes
ED25519_SIGNATURE_SIZE = 64


def _load_ed25519():
    """
    Attempt to load a real Ed25519 library.
    Returns (sign_fn, verify_fn) or (None, None) if unavailable.
    """
    try:
        # Prefer pure-Python ed25519 (no native compilation needed)
        # pip install ed25519
        import ed25519 as real_ed25519  # type: ignore

        def _sign(message: bytes, privkey: bytes) -> bytes:
            # ed25519 library uses 32-byte seed as private key
            signing_key = real_ed25519.SigningKey(privkey[:32])
            return signing_key.sign(message)

        def _verify(signature: bytes, message: bytes, pubkey: bytes) -> bool:
            verifying_key = real_ed25519.VerifyingKey(pubkey)
            try:
                verifying_key.verify(signature, message)
                return True
            except real_ed25519.BadSignatureError:
                return False

        return _sign, _verify
    except ImportError:
        pass

    try:
        # Fallback: cryptography (pip install cryptography)
        from cryptography.hazmat.primitives.asymmetric import ed25519 as crypto_ed25519
        from cryptography.exceptions import InvalidSignature

        def _sign(message: bytes, privkey: bytes) -> bytes:
            private_key = crypto_ed25519.Ed25519PrivateKey.from_private_bytes(privkey[:32])
            return private_key.sign(message)

        def _verify(signature: bytes, message: bytes, pubkey: bytes) -> bool:
            public_key = crypto_ed25519.Ed25519PublicKey.from_public_bytes(pubkey)
            try:
                public_key.verify(signature, message)
                return True
            except InvalidSignature:
                return False

        return _sign, _verify
    except ImportError:
        pass

    return None, None


def build_image(bootloader_path, app_path, output_path, signing_key_path=None):
    with open(app_path, 'rb') as f:
        app_data = f.read()

    with open(bootloader_path, 'rb') as f:
        bootloader_data = f.read()

    # 1. Bootloader (28KB)
    if len(bootloader_data) > 28672:
        print("ERROR: Bootloader is larger than 28KB!")
        sys.exit(1)

    padded_bootloader = bootloader_data.ljust(28672, b'\xFF')

    # 2. Partition Table (4KB)
    pt_data = struct.pack('<I II II II II',
        PT_MAGIC,
        0x00000000, 0x00007000, # Bootloader
        0x00008000, 0x00018000, # PART_A
        0x00020000, 0x00018000, # PART_B
        0x00038000, 0x00008000  # VFS
    )
    pt_crc = zlib.crc32(pt_data) & 0xFFFFFFFF
    pt_data += struct.pack('<I', pt_crc)
    padded_pt = pt_data.ljust(4096, b'\xFF')

    # 3. Active Image Header (128 Bytes)
    version = 1
    image_size = len(app_data)

    # --- Ed25519 Signature Generation ---
    if signing_key_path:
        ed25519_sign, _ = _load_ed25519()
        if ed25519_sign is None:
            print("ERROR: --signing-key requires an Ed25519 library.")
            print("  Install one of:  pip install ed25519   OR   pip install cryptography")
            sys.exit(1)

        with open(signing_key_path, 'rb') as f:
            private_key_bytes = f.read()
        if len(private_key_bytes) < 32:
            print(f"ERROR: Signing key file '{signing_key_path}' is too short "
                  f"({len(private_key_bytes)} bytes, expected 32+ bytes).")
            sys.exit(1)

        signature = ed25519_sign(app_data, private_key_bytes)
        print(f"  Signed with key '{signing_key_path}' "
              f"(Ed25519 signature: {len(signature)} bytes)")
    else:
        signature = bytes([0xED] * ED25519_SIGNATURE_SIZE)
        print("  WARNING: --signing-key not provided. Using MOCK signature (0xED-filled).")
        print("  This image will only boot if CONFIG_OTA_DEV_MODE is enabled on the device.")

    padding = bytes([0x00] * 48)

    header = struct.pack('<I I I I 64s 48s',
        FIRMWARE_MAGIC,
        version,
        image_size,
        VALID_STATUS,
        signature,
        padding
    )

    # Concatenate: Bootloader (28K) + PT (4K) + Header (128B) + App
    flash_data = padded_bootloader + padded_pt + header + app_data

    with open(output_path, 'wb') as f:
        f.write(flash_data)

    sig_type = "REAL Ed25519" if signing_key_path else "MOCK (0xED)"
    print(f"Success! Generated {output_path} (Size: {len(flash_data)} bytes)")
    print(f"App size: {image_size} bytes, Signature: {sig_type}")


if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description='Build auroraOS flash image with optional Ed25519 signing.'
    )
    parser.add_argument('bootloader', help='Path to bootloader binary (.bin)')
    parser.add_argument('app',        help='Path to auroraOS application binary (.bin)')
    parser.add_argument('output',     help='Path to output flash image (.bin)')
    parser.add_argument(
        '--signing-key', '-k',
        default=None,
        help='Path to Ed25519 private key file (32 bytes). '
             'If omitted, a MOCK 0xED-filled signature is embedded — '
             'this will only boot with CONFIG_OTA_DEV_MODE enabled.'
    )

    args = parser.parse_args()
    build_image(args.bootloader, args.app, args.output, args.signing_key)
