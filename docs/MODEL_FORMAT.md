# `.osiem` model format (version 1)

The intent models are small, so OSIE uses its own file format with a
dependency-free loader (`src/inference/neural_parser.cpp`) instead of an
inference runtime. The reasons are measured in `BENCHMARKS.md`. This page
specifies the format and the limits the loader enforces. **A model file is
untrusted input:** anything that violates this specification is rejected with
an error, and the process never crashes or allocates beyond the limits.

## Layout

```text
OSIEM1                       magic and version, first line exactly
<key> <values...>            header lines, one per line, space-separated
...
data                         the line "data" ends the header
<binary tensor data>         little-endian, in the order of the tensor lines
```

### Header keys

| Key | Values | Rule |
|---|---|---|
| `arch` | `bag` \| `cnn` \| `bigru` | required |
| `buckets` | integer | required; 1 ≤ n ≤ 2²² |
| `dim` | integer | required; embedding width; 1 ≤ n ≤ 4096 |
| `hidden` | integer | required; encoder width; 1 ≤ n ≤ 4096 |
| `ngram` | `min max` | optional (default 2 4); 1 ≤ min ≤ max ≤ 8 |
| `knowledge` | `0` \| `1` | optional; adds OS-lexicon category features |
| `hybrid_threshold` | decimal | optional; finite, in [0, 1] |
| `intents` | labels... | required; 1–128 unique labels |
| `tags` | labels... | required; 1–64 unique BIO labels (`O`, `B-PROC`, …) |
| `tensor` | `name dtype d0 [d1 [d2]]` | one per tensor; rank 1–3; each dᵢ in 1..2²² |

Parsing rules:

- Numbers are plain decimal digits: no sign, no trailing characters, no
  overflow. `-4`, `4abc` and `4294967300` are errors.
- Labels, tensor names and keys match `[A-Za-z0-9_-]{1,64}`.
- Each key may appear once, except `tensor`. Duplicate labels and duplicate
  tensor names are errors.
- An unknown key that is well formed is ignored, so newer writers can add
  metadata. A known key with the wrong number of values is an error.
- Intent labels that this build does not know map to `unsupported`.

### Tensor data

- `f32`: `prod(shape)` IEEE-754 single-precision floats.
- `i8`: allowed only for `emb`. It holds `shape[0]` f32 per-row scales, then
  `prod(shape)` int8 values, then zero padding to a 4-byte boundary measured
  from the start of the data section.

Every value must be finite with |x| ≤ 10⁶; this applies to f32 weights and
int8 row scales. The file must end exactly after the last tensor; at most 3
bytes of final int8 padding may be missing.

### Required tensors

The tensors and shapes must match the architecture exactly. A tensor that
the architecture does not use is an error.

| arch | encoder tensors | output tensors (enc = encoder width) |
|---|---|---|
| all | `emb [buckets, dim]` | `intent_w [intents, 2·enc]`, `intent_b [intents]`, `tag_w [tags, enc]`, `tag_b [tags]` |
| `bag` | `proj_w [hidden, dim]`, `proj_b [hidden]` | enc = hidden |
| `cnn` | `conv1_w [hidden, dim, 3]`, `conv1_b`, `conv2_w [hidden, hidden, 3]`, `conv2_b` | enc = hidden |
| `bigru` | `gru_{f,b}_{wih [3h, dim], whh [3h, h], bih [3h], bhh [3h]}` | enc = 2·hidden |

## Resource limits

| Limit | Value | Why |
|---|---|---|
| File size | 256 MiB | Checked from the file size before reading. The largest model trained so far (9.6M f32 parameters) is ≈ 37 MiB. |
| Header | 64 KiB, 256 lines | The header is metadata only. |
| Tensors | ≤ 32 | The largest architecture (`bigru`) needs 13. |
| Elements per tensor | ≤ 2²⁶ | Checked during multiplication, so it cannot overflow. |
| Weight magnitude | ≤ 10⁶ | Keeps every activation finite; see below. |

### Why |w| ≤ 10⁶ keeps the forward pass finite

Finite weights alone are not enough. Fuzzing found that weights near
`FLT_MAX` overflow to `inf` in a linear layer, and softmax then returns NaN.

Bound each layer with |w| ≤ 10⁶ and the widest models trained so far (bag
and CNN: dim ≤ 48, hidden ≤ 64; at most 128 inputs to the output layer):

- An embedding is a mean of rows, so it is ≤ 10⁶. For int8 it is ≤ 1.3 × 10⁸,
  from scale × 127.
- `bag`: projection ≤ 48 · 1.3×10⁸ · 10⁶ ≈ 6×10¹⁵, so logits ≤ 128 · 6×10¹⁵
  · 10⁶ ≈ 8×10²³.
- `cnn`: conv1 ≤ 48 · 3 · 1.3×10⁸ · 10⁶ ≈ 2×10¹⁶ and conv2 ≤ 64 · 3 · 2×10¹⁶
  · 10⁶ ≈ 4×10²⁴, so logits ≤ 128 · 4×10²⁴ · 10⁶ ≈ 5×10³².
- `bigru`: the hidden state is bounded by tanh/sigmoid (|h| ≤ 1), so logits
  are small at any width allowed here.

All of these are below the f32 maximum of 3.4×10³⁸. For wider models the
bound leaves less headroom. As a second guard, `softmax` returns a uniform
distribution if any logit is non-finite, which makes the hybrid parser fall
back to rules. Trained weights are many orders of magnitude below 10⁶: all 8
shipped models load under this bound.

## Where models are looked up

See [`INSTALL.md`](INSTALL.md#where-osie-looks-for-the-model):
1. `--model`
2. `$OSIE_MODEL`
3. the user data directory
4. the model installed with the binary
5. otherwise, the rule parser

The working directory is never searched.

## Tests

- `tests/unit/test_inference.cpp`:
  - hand-computed forward passes;
  - one test per rejection rule above;
  - a deterministic mutation test (8,000 mutated files);
  - regressions for the shape-overflow and huge-weight defects.
- `tests/fuzz/fuzz_model_loader.cpp`: a libFuzzer harness. Every accepted
  model must run a forward pass with finite probabilities. Seeds are in
  `tests/fuzz/seeds/`, and CI runs it for 5 minutes under ASan and UBSan.
