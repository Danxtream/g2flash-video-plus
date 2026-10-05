"""Read-only guards for firmware A's recovered 2.2.9.22 service ABI."""
import hashlib

RANGES = (
    (0x442896, 0x44295e, '97504ab1768b4e8f09d5b4ed8ab6099a2ce8f366d23ae605b73bf0f04ac1a473', 'static thread creation'),
    (0x4429b2, 0x4429ec, '473ebb70f8f7ae6b393096ad266c362b2c2dfd1cfb3029a2f2761e90aa0fee62', 'thread termination'),
    (0x442b2a, 0x442b4c, 'baf18a5d5907081834f25eca8f5a96bb9f5a1a4880c5da59d3f29fc808e2ad7c', 'thread delay'),
    (0x442b64, 0x442d44, '72d2cc8f5e04169446b1cd8d0a869857cf6a66c78e84162ee53d2e6669040f9a', 'timers'),
    (0x442ef6, 0x44307e, '575a6cb111758808137f48362b4830c4df18d80dacc121b1096f63e1ef254e7a', 'mutexes'),
    (0x458382, 0x458408, 'a3c4eff8699d4f689fbdf402f0a9c7e55098305d8d67aa93ef5af593b0aa3aa9', 'EvenHub allocation'),
    (0x48c1e8, 0x48c3a0, 'b6dc01bc86ed77b3b8d3f337e1073783bfa5a0c38b8eed3021e89fed6ed05953', 'generic heap allocation'),
)


def validate_stock(image):
    """Reject a donor whose pinned RTOS/heap code differs before patch generation."""
    bias = 0x379bfe
    for lo, hi, pin, description in RANGES:
        if hashlib.sha256(image[lo-bias:hi-bias]).hexdigest() != pin:
            raise ValueError(f'decoder speed {description} ABI mismatch at {lo:#x}')
