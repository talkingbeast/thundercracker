/* -*- mode: C; c-basic-offset: 4; intent-tabs-mode: nil -*-
 *
 * STIR -- Sifteo Tiled Image Reducer
 * M. Elizabeth Scott <beth@sifteo.com>
 *
 * Copyright <c> 2011 Sifteo, Inc. All rights reserved.
 */

#include "cppwriter.h"
#include "audioencoder.h"
#include <assert.h>
#include "sifteo/abi.h"

namespace Stir {

const char *CPPWriter::indent = "    ";

CPPWriter::CPPWriter(Logger &log, const char *filename)
    : mLog(log)
{
    if (filename) {
        mStream.open(filename);
        if (!mStream.is_open())
            log.error("Error opening output file '%s'", filename);
    }
    
    if (mStream.is_open())
        head();
}

void CPPWriter::head()
{
    mStream <<
        "/*\n"
        " * Generated by STIR. Do not edit by hand.\n"
        " */\n"
        "\n"
        "#include <sifteo/asset.h>\n";
}

void CPPWriter::foot()
{}
 
void CPPWriter::close()
{
    if (mStream.is_open()) {
        foot();
        mStream.close();
    }
}

void CPPWriter::writeString(const std::vector<uint8_t> &data)
{
    // Write a large uint8_t array, as a string literal. This is handled
    // more efficiently by the compiler than a large array, so the resulting
    // code compiles quickly. The downside: resulting code is very ugly!

    unsigned i = 0;
    
    do {
        unsigned lineLength = 2;
        mStream << "\"";

        while (i < data.size() && lineLength < 120) {
            uint8_t byte = data[i++];

            if (byte >= ' ' && byte <= '~' && byte != '"' && byte != '\\' && byte != '?') {
                mStream << (char)byte;
                lineLength++;
            } else {
                mStream << '\\'
                    << (char)('0' + (byte >> 6))
                    << (char)('0' + ((byte >> 3) & 7))
                    << (char)('0' + (byte & 7));
                lineLength += 4;
            }
        }

        mStream << "\"\n";
    } while (i < data.size());
}

void CPPWriter::writeArray(const std::vector<uint8_t> &data)
{
    char buf[8];
    mStream << indent;
    for (unsigned i = 0; i < data.size(); i++) {
        if (i && !(i % 16))
            mStream << "\n" << indent;
        sprintf(buf, "0x%02x,", data[i]);
        mStream << buf;
    }
    mStream << "\n";
}

void CPPWriter::writeArray(const std::vector<uint16_t> &data)
{
    char buf[8];
    mStream << indent;
    for (unsigned i = 0; i < data.size(); i++) {
        if (i && !(i % 8))
            mStream << "\n" << indent;
        sprintf(buf, "0x%04x,", data[i]);
        mStream << buf;
    }
    mStream << "\n";
}

CPPSourceWriter::CPPSourceWriter(Logger &log, const char *filename)
    : CPPWriter(log, filename), nextGroupOrdinal(0) {}

void CPPSourceWriter::writeGroup(const Group &group)
{
    char hash[32];

#ifdef __MINGW32__
    sprintf(hash, "0x%016I64x", (long long unsigned int) group.getHash());
#else
    sprintf(hash, "0x%016llx", (long long unsigned int) group.getHash());
#endif

    /*
     * XXX: This method of generating the group Ordinal only works within
     *      a single Stir run. Ideally we'd be able to use _SYS_lti_counter
     *      or equivalent, but there's no efficient way to stick that in
     *      read-only data yet.
     */

    mStream <<
        "\n"
        "static const struct {\n" <<
        indent << "struct _SYSAssetGroupHeader hdr;\n" <<
        indent << "uint8_t data[" << group.getLoadstream().size() << "];\n"
        "} " << group.getName() << "_data = {{\n" <<
        indent << "/* reserved  */ 0,\n" <<
        indent << "/* ordinal   */ " << nextGroupOrdinal++ << ",\n" <<
        indent << "/* numTiles  */ " << group.getPool().size() << ",\n" <<
        indent << "/* dataSize  */ " << group.getLoadstream().size() << ",\n" <<
        indent << "/* hash      */ " << hash << ",\n"
        "}, {\n";

    writeArray(group.getLoadstream());

    mStream <<
        "}};\n\n"
        "Sifteo::AssetGroup " << group.getName() << " = {{\n" <<
        indent << "/* pHdr      */ reinterpret_cast<uint32_t>(&" << group.getName() << "_data.hdr),\n" <<
        "}};\n\n";

    mLog.infoBegin("Encoding images");
    for (std::set<Image*>::iterator i = group.getImages().begin();
         i != group.getImages().end(); i++)
        writeImage(**i);
    mLog.infoEnd();
}

void CPPSourceWriter::writeSound(const Sound &sound)
{
    std::vector<uint8_t> data;
    AudioEncoder *enc = AudioEncoder::create(sound.getEncode());
    assert(enc != 0);

    enc->encodeFile(sound.getFile(), data);
    mLog.infoLineWithLabel(sound.getName().c_str(),
        "%7.02f kiB, %s (%s)",
        data.size() / 1024.0f, enc->getName(), sound.getFile().c_str());

    if (data.empty()) {
        mLog.error("Error encoding audio file '%s'", sound.getFile().c_str());
        delete enc;
        return;
    }

    mStream << "static const char " << sound.getName() << "_data[] = \n";
    writeString(data);
    mStream << ";\n\n";

    // If the loop length is 0, there is no looping by default.
    _SYSAudioLoopType loopType = sound.getLoopType();
    if (sound.getLoopLength() == 0)
        loopType = _SYS_LOOP_ONCE;

    // Precompute the number of samples in the clip
    uint32_t numSamples = data.size();
    switch (enc->getType()) {
        case _SYS_PCM:
            if (numSamples & 1) {
                mLog.error("File '%s' does not contain an integral number of samples.",
                           sound.getFile().c_str());
            }
            numSamples /= sizeof(int16_t);
            break;
        case _SYS_ADPCM:
            numSamples *= 2;
            break;
    }

    // Compute loop length and check sanity
    uint32_t loopEnd;
    if (sound.getLoopLength() == 0) {
        loopEnd = numSamples;
    } else {
        loopEnd = sound.getLoopLength() + sound.getLoopStart();
    }
    // loopEnd is zero-indexed
    loopEnd -= 1;

    // Loop end bounds checking
    assert(loopEnd < numSamples);
    assert(sound.getLoopStart() < loopEnd);

    mStream <<
        "extern const Sifteo::AssetAudio " << sound.getName() << " = {{\n" <<
        indent << "/* sampleRate */ " << sound.getSampleRate() << ",\n" <<
        indent << "/* loopStart  */ " << sound.getLoopStart() << ",\n" <<
        indent << "/* loopEnd    */ " << loopEnd << ",\n" <<
        indent << "/* loopType   */ " << (loopType == _SYS_LOOP_ONCE ? "_SYS_LOOP_ONCE" : "_SYS_LOOP_REPEAT") << ",\n" <<
        indent << "/* type       */ " << enc->getTypeSymbol() << ",\n" <<
        indent << "/* volume     */ " << sound.getVolume() << ",\n" <<
        indent << "/* dataSize   */ " << data.size() << ",\n" <<
        indent << "/* pData      */ reinterpret_cast<uint32_t>(" << sound.getName() << "_data),\n" <<
        "}};\n\n";

    delete enc;
}

void CPPSourceWriter::writeImage(const Image &image)
{
    const std::vector<TileGrid> &grids = image.getGrids();
    unsigned width = grids.empty() ? 0 : grids[0].width();
    unsigned height = grids.empty() ? 0 : grids[0].height();

    // Declare the data so we can do a forward reference,
    // to keep the header ordered first in memory when we can.
    mStream << "extern const uint16_t " << image.getName() << "_data[];\n";

    // This header can often be optimized out by slinky, unless its address is taken.
    // Here we output just the common non-format-specific header.
    mStream <<
        "\n"
        "extern const Sifteo::" << image.getClassName() << " " << image.getName() << " = {{\n" <<
        indent << "/* group    */ reinterpret_cast<uint32_t>(&" << image.getGroup()->getName() << "),\n" <<
        indent << "/* width    */ " << width << ",\n" <<
        indent << "/* height   */ " << height << ",\n" <<
        indent << "/* frames   */ " << grids.size() << ",\n";

    bool autoFormat = !(image.isPinned() || image.isFlat());
    bool isSingleTile = width == 1 && height == 1 && grids.size() == 1;

    if (image.isPinned() || (autoFormat && isSingleTile)) {
        mStream <<
            indent << "/* format   */ _SYS_AIF_PINNED,\n" <<
            indent << "/* reserved */ 0,\n" <<
            indent << "/* pData    */ " << image.encodePinned() << "\n}};\n\n";
        return;
    }
    
    // Try to compress the asset, if it isn't explicitly flat.
    if (autoFormat) {
        std::vector<uint16_t> data;
        std::string format;
        if (image.encodeDUB(data, mLog, format)) {
            mStream <<
                indent << "/* format   */ " << format << ",\n" <<
                indent << "/* reserved */ 0,\n" <<
                indent << "/* pData    */ reinterpret_cast<uint32_t>(" << image.getName() << "_data)\n}};\n\n" <<
                "const uint16_t " << image.getName() << "_data[] = {\n";
            writeArray(data);
            mStream << "};\n\n";
            return;
        }
    }

    // Fall back on a Flat (uncompressed tile array) asset. Note that we only
    // wrap this in a FlatAssetImage class if the script explicitly requested
    // a flat asset. If we decided not to compress an asset with default params,
    // it will still be in an AssetImage class, but the compression format will
    // be _SYS_AIF_FLAT.

    mStream <<
        indent << "/* format   */ _SYS_AIF_FLAT,\n" <<
        indent << "/* reserved */ 0,\n" <<
        indent << "/* pData    */ reinterpret_cast<uint32_t>(" << image.getName() << "_data)\n}};\n\n" <<
        "const uint16_t " << image.getName() << "_data[] = {\n";
    std::vector<uint16_t> data;
    image.encodeFlat(data);
    writeArray(data);
    mStream << "};\n\n";
}

void CPPSourceWriter::writeTracker(const Tracker &tracker)
{
    const _SYSXMSong &song = tracker.getSong();

    // Samples:
    for (unsigned i = 0; i < song.nInstruments; i++) {
        const _SYSXMInstrument &instrument = tracker.getInstrument(i);

        if (instrument.sample.pData < song.nInstruments) {
            const std::vector<uint8_t> &buf = tracker.getSample(instrument.sample.pData);
            mStream << "static const char " << tracker.getName() << "_instrument" << i << "_sampleData[] = " <<
                       "// " << buf.size() << " bytes\n";
            writeString(buf);
            mStream << ";\n\n";
        }
    }

    // Envelopes:
    for (unsigned i = 0; i < song.nInstruments; i++) {
        const _SYSXMInstrument &instrument = tracker.getInstrument(i);

        // Then the envelope:
        if (instrument.volumeEnvelopePoints < song.nInstruments) {
            const std::vector<uint8_t> &buf = tracker.getEnvelope(instrument.volumeEnvelopePoints);
            mStream << "static const char " << tracker.getName() << "_instrument" << i << "_envelope[] = " <<
                       "// " << buf.size() << " bytes\n";
            writeString(buf);
            mStream << ";\n\n";
        }
    }

    // Instrument array:
    mStream << "static const _SYSXMInstrument " << tracker.getName() << "_instruments[] = {";
    for (unsigned i = 0; i < song.nInstruments; i++) {
        const _SYSXMInstrument &instrument = tracker.getInstrument(i);

        mStream <<
        "\n{ // Instrument " << i << "\n" <<
        indent << "/* sample */ {\n" <<
        indent << indent << "/* sampleRate */ " << instrument.sample.sampleRate << ",\n" <<
        indent << indent << "/* loopStart  */ " << instrument.sample.loopStart << ",\n" <<
        indent << indent << "/* loopEnd    */ " << instrument.sample.loopEnd << ",\n" <<
        indent << indent << "/* loopType   */ " << (instrument.sample.loopType == _SYS_LOOP_ONCE ? "_SYS_LOOP_ONCE" : "_SYS_LOOP_REPEAT") << ",\n" <<
        indent << indent << "/* type       */ " << (uint32_t)instrument.sample.type << ",\n" <<
        indent << indent << "/* volume     */ " << instrument.sample.volume << ",\n" <<
        indent << indent << "/* dataSize   */ " << instrument.sample.dataSize << ",\n" <<
        indent << indent << "/* pData      */ reinterpret_cast<uint32_t>(" << tracker.getName() << "_instrument" << i << "_sampleData),\n" <<
        indent << "},\n" <<
        indent << "/* finetune              */ " << (int32_t)instrument.finetune << ",\n" <<
        indent << "/* relativeNoteNumber    */ " << (int32_t)instrument.relativeNoteNumber << ",\n" <<
        indent << "/* volumeEnvelopePoints  */ reinterpret_cast<uint32_t>(" << tracker.getName() << "_instrument" << i << "_envelope),\n" <<
        indent << "/* nVolumeEnvelopePoints */ " << (uint32_t)instrument.nVolumeEnvelopePoints << ",\n" <<
        indent << "/* volumeSustainPoint    */ " << (uint32_t)instrument.volumeSustainPoint << ",\n" <<
        indent << "/* volumeLoopStartPoint  */ " << (uint32_t)instrument.volumeLoopStartPoint << ",\n" <<
        indent << "/* volumeLoopEndPoint    */ " << (uint32_t)instrument.volumeLoopEndPoint << ",\n" <<
        indent << "/* volumeType            */ " << (uint32_t)instrument.volumeType << ",\n" <<
        indent << "/* vibratoType           */ " << (uint32_t)instrument.vibratoType << ",\n" <<
        indent << "/* vibratoSweep          */ " << (uint32_t)instrument.vibratoSweep << ",\n" <<
        indent << "/* vibratoDepth          */ " << (uint32_t)instrument.vibratoDepth << ",\n" <<
        indent << "/* vibratoRate           */ " << (uint32_t)instrument.vibratoRate << ",\n" <<
        indent << "/* volumeFadeout         */ " << instrument.volumeFadeout << ",\n" <<
        "},";
    }
    mStream << "};\n\n";

    // Pattern data:
    for (unsigned i = 0; i < song.nPatterns; i++) {
        const std::vector<uint8_t> &buf = tracker.getPatternData(i);
        mStream << "static const char " << tracker.getName() << "_pattern" << i << "_data[] = " <<
                   "// " << buf.size() << " bytes\n";
        writeString(buf);
        mStream << ";\n\n";
    }

    // Patterns:
    mStream << "static const _SYSXMPattern " << tracker.getName() << "_patterns[] = {";
    for (unsigned i = 0; i < song.nPatterns; i++) {
        const _SYSXMPattern &pattern = tracker.getPattern(i);
        
        mStream <<
        "\n{ // Pattern " << i << "\n" <<
        indent << "/* nRows     */ " << (uint32_t)pattern.nRows << ",\n" <<
        indent << "/* dataSize  */ " << (uint32_t)pattern.dataSize << ",\n" <<
        indent << "/* pData     */ reinterpret_cast<uint32_t>(" << tracker.getName() << "_pattern" << i << "_data),\n" <<
        "},";
    }
    mStream << "};\n\n";

    // Pattern table:
    {
        const std::vector<uint8_t> &buf = tracker.getPatternTable();
        mStream << "static const char " << tracker.getName() << "_patternOrderTable[] = " <<
                   "// " << buf.size() << " bytes\n";
        writeString(buf);
        mStream << ";\n\n";
    }

    // Song:
    mStream <<
    "extern const Sifteo::AssetTracker " << tracker.getName() << " = {{\n" <<
    indent << "/* patternOrderTable     */ reinterpret_cast<uint32_t>(" << tracker.getName() << "_patternOrderTable),\n" <<
    indent << "/* patternOrderTableSize */ " << song.patternOrderTableSize << ",\n" <<
    indent << "/* restartPosition       */ " << (uint32_t)song.restartPosition << ",\n" <<
    indent << "/* nChannels             */ " << (uint32_t)song.nChannels << ",\n" <<
    indent << "/* nPatterns             */ " << song.nPatterns << ",\n" <<
    indent << "/* patterns              */ reinterpret_cast<uint32_t>(" << tracker.getName() << "_patterns),\n" <<
    indent << "/* nInstruments          */ " << (uint32_t)song.nInstruments << ",\n" <<
    indent << "/* instruments           */ reinterpret_cast<uint32_t>(" << tracker.getName() << "_instruments),\n" <<
    indent << "/* frequencyTable        */ " << (uint32_t)song.frequencyTable << ",\n" <<
    indent << "/* tempo                 */ " << song.tempo << ",\n" <<
    indent << "/* bpm                   */ " << song.bpm << ",\n" <<
    "}};\n\n";

    unsigned compressedSize = tracker.getSize();
    unsigned uncompressedSize = tracker.getFileSize();
    double ratio = uncompressedSize ? 100.0 - compressedSize * 100.0 / uncompressedSize : 0;

    mLog.infoLineWithLabel(tracker.getName().c_str(), "% 3u patterns,% 3u instruments, %5.02f kiB, % 5.01f%% compression (%s)",
                 song.nPatterns,
                 song.nInstruments,
                 compressedSize / 1024.0f,
                 ratio,
                 tracker.getFile().c_str());
}

CPPHeaderWriter::CPPHeaderWriter(Logger &log, const char *filename)
    : CPPWriter(log, filename)
{
    if (filename)
        createGuardName(filename);

    if (mStream.is_open())
        head();
}

void CPPHeaderWriter::createGuardName(const char *filename)
{
    /*
     * Make a name for the include guard, based on the filename
     */

    char c;
    char prev = '_';
    guardName = prev;

    while ((c = *filename)) {
        c = toupper(c);

        if (isalpha(c)) {
            prev = c;
            guardName += prev;
        } else if (prev != '_') {
            prev = '_';
            guardName += prev;
        }

        filename++;
    }
}

void CPPHeaderWriter::head()
{
    mStream <<
        "\n"
        "#ifndef " << guardName << "\n"
        "#define " << guardName << "\n"
        "\n";
}

void CPPHeaderWriter::foot()
{
    mStream <<
        "\n"
        "#endif  // " << guardName << "\n";

    CPPWriter::foot();
}

void CPPHeaderWriter::writeGroup(const Group &group)
{
    mStream << "extern Sifteo::AssetGroup " << group.getName() << ";\n";

    for (std::set<Image*>::iterator i = group.getImages().begin();
         i != group.getImages().end(); i++) {
        Image *image = *i;
        mStream << "extern const Sifteo::" << image->getClassName() << " " << image->getName() << ";\n";
    }
}

void CPPHeaderWriter::writeSound(const Sound &sound)
{
    mStream << "extern const Sifteo::AssetAudio " << sound.getName() << ";\n";
}

void CPPHeaderWriter::writeTracker(const Tracker &tracker)
{
    mStream << "extern const Sifteo::AssetTracker " << tracker.getName() << ";\n";
}

};  // namespace Stir
