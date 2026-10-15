# Palm OS 4 Note Pad drawings and Google Drive

[Documentation](../README.md) · [Shared Google sign-in](README.md)

The Note Pad conduit synchronizes standard **Palm OS 4 npadDB** drawings in both directions with **PNG files** in **My Drive → Palm → &lt;HotSync user name&gt; → Notes**. Incoming PNGs become native, editable Note Pad drawings. This is separate from Memo Pad text sync and whole-database Drive backup.

## Setup and use

1. Enable **Google Drive API** in the Cloud project used by the shared Google sign-in.
2. Open **wifi.setup → Note Pad**, reuse the existing Drive grant, select the Palm title encoding and enable sync. No Keep permission is required.
3. If the Palm has no HotSync user name, save one in **wifi.setup → HotSync settings**. Check Google access, disconnect Network, then tap standalone HotSync. The adapter fills the missing name first. The access check tests Drive authorization/listing; HotSync validates the actual Palm OS 4 records and PNG contents.
4. Open the Notes folder in Drive. Palm drawing titles become PNG file names. Add a PNG there to create a new Palm note. Download/edit a PNG in an image editor and replace the same Drive file/version to update its linked Palm drawing. Renaming the PNG updates the Palm title.

Palm-side edits update the same Drive file. Renaming a drawing on Palm updates its Google file name. Stable file IDs distinguish edits from newly added notes; identical names alone do not merge unrelated files. Keep the `.png` extension.

## Image conversion and preserved Palm data

- Standard OS 4 raw and run-length encoded monochrome drawings are decoded. Scrolling canvases taller than one screen are preserved in full. Widths up to 160 pixels and up to **32,000 decoded bitmap bytes** are supported (for example, 160×1600 at full width); the OS 4 stored width excludes eight padding pixels, as documented by pilot-link.
- Native RLE compatibility follows the reference exporter: zero-count runs are no-ops, and a single unpaired final byte is ignored only when complete pairs already decode the entire canvas. Missing pixels, run overruns, payload lengths beyond the received record and nonzero data outside the declared payload still stop sync. Errors include the record ID, received size, canvas dimensions, payload length and decoded/expected bitmap bytes.
- PNG imports must be **non-interlaced**, at most **160 pixels wide**, **32,000 decoded bitmap bytes** and **64 KiB per file**. Supported formats are 1/2/4/8-bit grayscale or palette images and 8-bit RGB, RGBA or grayscale-alpha. Palette/transparency data, PNG filters, CRCs and the compressed stream are validated. Unsupported formats stop with an explicit error.
- Each PNG is fetched in one bounded, revision-locked HTTPS request. The adapter receives 1 KiB blocks, closes TLS, then assembles the file. It does not reserve the whole file before connecting or establish another TLS connection beside every accumulated 4 KiB chunk. The binary response cap is the advertised file size, at most 64 KiB; ordinary Docs/JSON requests retain their existing 32 KiB cap. Incomplete or mismatched ranges stop sync before note transfers.
- Import memory also depends on the filtered raster and available contiguous ESP heap. The decoder reads compressed data directly from validated PNG chunks, without a second compressed copy. Its decompression window is the filtered raster size for small images, capped at 32 KiB for larger images. For example, a 160×160 one-bit image uses a 3,360-byte window, compared with 25,760 bytes for eight-bit grayscale; both also need decoder state, bitmap and file storage. Prefer a one-bit black-and-white PNG for the lowest memory use. The file/bitmap limits are upper bounds, not a guarantee that every allowed image fits alongside an existing note collection.
- HotSync record buffers grow only when a native drawing needs more space and shrink after the Palm snapshot and each write, before later Google requests. A buffer allocation failure stops the transfer with an error. Native RLE output is written directly into that buffer without a second encoded-bitmap copy. The conduit retains one projected remote bitmap and avoids extra full drawing copies for merge/upload. The existing conservative combined content budget still applies, so a large book can exceed it even when individual images fit.
- Imported pixels are converted to black and white using a luminance threshold; transparency is composited onto white. Widths are padded to whole bytes. This is not a lossless color-image round trip.
- On an existing Palm note, importing a title or drawing edit preserves its creation date, alarm and category, while updating its change date. New imported drawings have no alarm and use the Unfiled category. A drawing without a title is exported with an `Untitled note` file name.
- Google color/extra image information survives a title-only Palm rename. A Palm drawing edit uploads its monochrome representation. Exported PNGs use standard one-bit grayscale and need little compression workspace.
- Palm OS 5 PNG-backed Note Pad records, high-resolution canvases, third-party drawing apps and newer note formats are not covered by this OS 4 codec. Their native database can still be handled by Drive backup when it meets that feature's format limits.

## Sync behavior and recovery

Titles and pixels are tracked separately. Independent changes merge; conflicting edits to the same field stop for review by default. The portal can instead prefer Palm or Google edits. This also resolves deletion-versus-edit conflicts. Google revision/metadata checks guard uploads and renames against concurrent edits. File versions are rechecked before deletion; cross-device operations are not atomic.

Explicit Palm deletion tombstones move linked PNGs to Drive Trash. Confirmed Google deletion removes the Palm note. Moving a file outside the Notes folder pauses the link. Private and archived Palm records are excluded from application sync; their mappings remain reserved so they cannot return as public notes.

Uploads save a preassigned Drive ID before sending bytes. Incoming Palm record IDs are reserved before installation. Lost replies or interrupted state writes can therefore be reconciled without creating another note. Do not erase the adapter's saved mappings while a transfer is pending. Keep native backups for recovery; PNG files do not contain Palm alarm/category metadata.

Defaults allow 64 Palm records/folder notes, a combined 64,000-byte content budget and a 24,000-byte folder-metadata budget. The shared NVS partition also stores the other conduits' settings and mappings; a full partition reports an error instead of silently accepting an incomplete sync. See [configuration](../configuration.md).

The user confirmed Note Pad working on Palm OS 4 on 15 October 2026 after the scrolling-canvas and RLE compatibility fixes, then reported PNG decode memory exhaustion and HTTPS admission failure with a 160×352 grayscale PNG. The user subsequently confirmed Note Pad working after the PNG decoder/window and HTTPS download/buffer-lifetime fixes on 15 October 2026. Those fixes also have firmware-build and sanitizer-enabled host coverage. Regressions import a >56 KiB 160×352 grayscale PNG in one request, validate the complete native image, reject short downloads without Palm/cloud changes, release native record buffers before downloads, and preserve ordinary JSON response caps. Dedicated tests exercise the firmware's miniz path with the SDK's portable implementation, allocation limits/failures, eight-bit grayscale, RGB, split PNG data chunks and window wrapping. The two-way sync tests independently use zlib; firmware executes the C3 ROM miniz inflater. The full range of image sizes, recovery and conflict cases has not been separately confirmed on hardware.

Protocol reference: [pilot-link Note Pad decoder](https://github.com/desrod/pilot-link/blob/master/libpisock/notepad.c) and [image export](https://github.com/desrod/pilot-link/blob/master/src/pilot-read-notepad.c).
