/*
 * pedalboard
 * Copyright 2022 Spotify AB
 *
 * Licensed under the GNU Public License, Version 3.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    https://www.gnu.org/licenses/gpl-3.0.html
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <mutex>
#include <optional>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include "../BufferUtils.h"
#include "../JuceHeader.h"

namespace py = pybind11;

namespace Pedalboard {

static constexpr const unsigned int DEFAULT_AUDIO_BUFFER_SIZE_FRAMES = 8192;

class AudioFile {};

class ReadableAudioFile
    : public AudioFile,
      public std::enable_shared_from_this<ReadableAudioFile> {
public:
  ReadableAudioFile(std::string filename) : filename(filename) {
    formatManager.registerBasicFormats();
    juce::File file(filename);

    if (!file.existsAsFile()) {
      throw std::domain_error(
          "Failed to open audio file: file does not exist: " + filename);
    }

    // createReaderFor(juce::File) is fast, as it only looks at file extension:
    reader.reset(formatManager.createReaderFor(file));
    if (!reader) {
      // This is slower but more thorough:
      reader.reset(formatManager.createReaderFor(file.createInputStream()));

      // Known bug: the juce::MP3Reader class will parse formats that are not
      // MP3 and pretend like they are, producing garbage output. For now, if we
      // parse MP3 from an input stream that's not explicitly got ".mp3" on the
      // end, ignore it.
      if (reader && reader->getFormatName() == "MP3 file") {
        throw std::domain_error(
            "Failed to open audio file: file \"" + filename +
            "\" does not seem to be of a known or supported format. (If trying "
            "to open an MP3 file, ensure the filename ends with '.mp3'.)");
      }
    }

    if (!reader)
      throw std::domain_error(
          "Failed to open audio file: file \"" + filename +
          "\" does not seem to be of a known or supported format.");
  }

  double getSampleRate() const {
    if (!reader)
      throw std::runtime_error("I/O operation on a closed file.");
    return reader->sampleRate;
  }

  long getLengthInSamples() const {
    if (!reader)
      throw std::runtime_error("I/O operation on a closed file.");
    return reader->lengthInSamples;
  }

  double getDuration() const {
    if (!reader)
      throw std::runtime_error("I/O operation on a closed file.");
    return reader->lengthInSamples / reader->sampleRate;
  }

  long getNumChannels() const {
    if (!reader)
      throw std::runtime_error("I/O operation on a closed file.");
    return reader->numChannels;
  }

  std::string getFileFormat() const {
    if (!reader)
      throw std::runtime_error("I/O operation on a closed file.");

    return reader->getFormatName().toStdString();
  }

  std::string getFileDatatype() const {
    if (!reader)
      throw std::runtime_error("I/O operation on a closed file.");

    if (reader->usesFloatingPointData) {
      switch (reader->bitsPerSample) {
      case 16: // OGG returns 16-bit int data, but internally stores floats
      case 32:
        return "float32";
      case 64:
        return "float64";
      default:
        return "unknown";
      }
    } else {
      switch (reader->bitsPerSample) {
      case 8:
        return "int8";
      case 16:
        return "int16";
      case 24:
        return "int24";
      case 32:
        return "int32";
      case 64:
        return "int64";
      default:
        return "unknown";
      }
    }
  }

  py::array_t<float> read(long long numSamples) {
    if (numSamples == 0)
      throw std::domain_error(
          "ReadableAudioFile will not read an entire file at once, due to the "
          "possibility that a file may be larger than available memory. Please "
          "pass a number of frames to read (available from the 'frames' "
          "attribute).");

    const juce::ScopedLock scopedLock(objectLock);
    if (!reader)
      throw std::runtime_error("I/O operation on a closed file.");

    // Allocate a buffer to return of up to numSamples:
    int numChannels = reader->numChannels;
    numSamples =
        std::min(numSamples, reader->lengthInSamples - currentPosition);
    py::array_t<float> buffer =
        py::array_t<float>({(int)numChannels, (int)numSamples});

    py::buffer_info outputInfo = buffer.request();

    {
      py::gil_scoped_release release;

      // If the file being read does not have enough content, it _should_ pad
      // the rest of the array with zeroes. Unfortunately, this does not seem to
      // be true in practice, so we pre-zero the array to be returned here:
      std::memset((void *)outputInfo.ptr, 0,
                  numChannels * numSamples * sizeof(float));

      float **channelPointers = (float **)alloca(numChannels * sizeof(float *));
      for (int c = 0; c < numChannels; c++) {
        channelPointers[c] = ((float *)outputInfo.ptr) + (numSamples * c);
      }

      if (reader->usesFloatingPointData || reader->bitsPerSample == 32) {
        if (!reader->read(channelPointers, numChannels, currentPosition,
                          numSamples)) {
          throw std::runtime_error("Failed to read from file.");
        }
      } else {
        // If the audio is stored in an integral format, read it as integers
        // and do the floating-point conversion ourselves to work around
        // floating-point imprecision in JUCE when reading formats smaller than
        // 32-bit (i.e.: 16-bit audio is off by about 0.003%)

        if (!reader->readSamples((int **)channelPointers, numChannels, 0,
                                 currentPosition, numSamples)) {
          throw std::runtime_error("Failed to read from file.");
        }

        // When converting 24-bit, 16-bit, or 8-bit data from int to float,
        // the values provided by the above read() call are shifted left
        // (such that the least significant bits are all zero)
        // JUCE will then divide these values by 0x7FFFFFFF, even though
        // the least significant bits are zero, effectively losing precision.
        // Instead, here we set the scale factor appropriately.
        int maxValueAsInt;
        switch (reader->bitsPerSample) {
        case 24:
          maxValueAsInt = 0x7FFFFF00;
          break;
        case 16:
          maxValueAsInt = 0x7FFF0000;
          break;
        case 8:
          maxValueAsInt = 0x7F000000;
          break;
        default:
          throw std::runtime_error("Not sure how to convert data from " +
                                   std::to_string(reader->bitsPerSample) +
                                   " bits per sample to floating point!");
        }
        float scaleFactor = 1.0f / static_cast<float>(maxValueAsInt);

        for (int c = 0; c < numChannels; c++) {
          juce::FloatVectorOperations::convertFixedToFloat(
              channelPointers[c], (const int *)channelPointers[c], scaleFactor,
              numSamples);
        }
      }
    }

    currentPosition += numSamples;
    return buffer;
  }

  py::handle readRaw(long long numSamples) {
    if (numSamples == 0)
      throw std::domain_error(
          "ReadableAudioFile will not read an entire file at once, due to the "
          "possibility that a file may be larger than available memory. Please "
          "pass a number of frames to read (available from the 'frames' "
          "attribute).");

    const juce::ScopedLock scopedLock(objectLock);
    if (!reader)
      throw std::runtime_error("I/O operation on a closed file.");

    if (reader->usesFloatingPointData) {
      return read(numSamples).release();
    } else {
      switch (reader->bitsPerSample) {
      case 32:
        return readInteger<int>(numSamples).release();
      case 16:
        return readInteger<short>(numSamples).release();
      case 8:
        return readInteger<char>(numSamples).release();
      default:
        throw std::runtime_error("Not sure how to read " +
                                 std::to_string(reader->bitsPerSample) +
                                 "-bit audio data!");
      }
    }
  }

  template <typename SampleType>
  py::array_t<SampleType> readInteger(long long numSamples) {
    if (reader->usesFloatingPointData) {
      throw std::runtime_error(
          "Can't call readInteger with a floating point file!");
    }

    // Allocate a buffer to return of up to numSamples:
    int numChannels = reader->numChannels;
    numSamples =
        std::min(numSamples, reader->lengthInSamples - currentPosition);
    py::array_t<SampleType> buffer =
        py::array_t<SampleType>({(int)numChannels, (int)numSamples});

    py::buffer_info outputInfo = buffer.request();

    {
      py::gil_scoped_release release;
      if (reader->bitsPerSample > 16) {
        if (sizeof(SampleType) < 4) {
          throw std::runtime_error("Output array not wide enough to store " +
                                   std::to_string(reader->bitsPerSample) +
                                   "-bit integer data.");
        }

        std::memset((void *)outputInfo.ptr, 0,
                    numChannels * numSamples * sizeof(SampleType));

        int **channelPointers = (int **)alloca(numChannels * sizeof(int *));
        for (int c = 0; c < numChannels; c++) {
          channelPointers[c] = ((int *)outputInfo.ptr) + (numSamples * c);
        }

        if (!reader->readSamples(channelPointers, numChannels, 0,
                                 currentPosition, numSamples)) {
          throw std::runtime_error("Failed to read from file.");
        }
      } else {
        // Read the file in smaller chunks, converting from int32 to the
        // appropriate output format as we go:
        std::vector<std::vector<int>> intBuffers;
        intBuffers.resize(numChannels);

        int **channelPointers = (int **)alloca(numChannels * sizeof(int *));
        for (long long startSample = 0; startSample < numSamples;
             startSample += DEFAULT_AUDIO_BUFFER_SIZE_FRAMES) {
          int samplesToRead =
              std::min(numSamples - startSample,
                       (long long)DEFAULT_AUDIO_BUFFER_SIZE_FRAMES);

          for (int c = 0; c < numChannels; c++) {
            intBuffers[c].resize(samplesToRead);
            channelPointers[c] = intBuffers[c].data();
          }

          if (!reader->readSamples(channelPointers, numChannels, 0,
                                   currentPosition + startSample,
                                   samplesToRead)) {
            throw std::runtime_error("Failed to read from file.");
          }

          // Convert the data in intBuffers to the output format:
          char shift = 32 - reader->bitsPerSample;
          for (int c = 0; c < numChannels; c++) {
            SampleType *outputChannelPointer =
                (((SampleType *)outputInfo.ptr) + (c * numSamples));
            for (int i = 0; i < samplesToRead; i++) {
              outputChannelPointer[startSample + i] = intBuffers[c][i] >> shift;
            }
          }
        }
      }
    }

    currentPosition += numSamples;
    return buffer;
  }

  void seek(long long targetPosition) {
    const juce::ScopedLock scopedLock(objectLock);
    if (!reader)
      throw std::runtime_error("I/O operation on a closed file.");
    if (targetPosition > reader->lengthInSamples)
      throw std::domain_error("Cannot seek beyond end of file (" +
                              std::to_string(reader->lengthInSamples) +
                              " frames).");
    if (targetPosition < 0)
      throw std::domain_error("Cannot seek before start of file.");
    currentPosition = targetPosition;
  }

  long long tell() const {
    const juce::ScopedLock scopedLock(objectLock);
    if (!reader)
      throw std::runtime_error("I/O operation on a closed file.");
    return currentPosition;
  }

  void close() {
    const juce::ScopedLock scopedLock(objectLock);
    reader.reset();
  }

  bool isClosed() const {
    const juce::ScopedLock scopedLock(objectLock);
    return !reader;
  }

  bool isSeekable() const {
    const juce::ScopedLock scopedLock(objectLock);

    // At the moment, ReadableAudioFile instances are always seekable, as
    // they're backed by files.
    return !isClosed();
  }

  std::string getFilename() const { return filename; }

  std::shared_ptr<ReadableAudioFile> enter() { return shared_from_this(); }

  void exit(const py::object &type, const py::object &value,
            const py::object &traceback) {
    close();
  }

private:
  juce::AudioFormatManager formatManager;
  std::string filename;
  std::unique_ptr<juce::AudioFormatReader> reader;
  juce::CriticalSection objectLock;

  int currentPosition = 0;
};

bool isInteger(double value) {
  double intpart;
  return modf(value, &intpart) == 0.0;
}

int determineQualityOptionIndex(juce::AudioFormat *format,
                                const std::string inputString) {
  // Detect the quality level to use based on the string passed in:
  juce::StringArray possibleQualityOptions = format->getQualityOptions();
  int qualityOptionIndex = -1;

  std::string qualityString = juce::String(inputString).trim().toStdString();

  if (!qualityString.empty()) {
    if (!possibleQualityOptions.size()) {
      throw std::domain_error("Unable to parse provided quality value (" +
                              qualityString + "). " +
                              format->getFormatName().toStdString() +
                              "s do not accept quality settings.");
    }

    // Try to match the string against the available options. An exact match
    // is preferred (ignoring case):
    if (qualityOptionIndex == -1 &&
        possibleQualityOptions.contains(qualityString, true)) {
      qualityOptionIndex = possibleQualityOptions.indexOf(qualityString, true);
    }

    // And if no exact match was found, try casting to an integer:
    if (qualityOptionIndex == -1) {
      int numLeadingDigits = 0;
      for (int i = 0; i < qualityString.size(); i++) {
        if (juce::CharacterFunctions::isDigit(qualityString[i])) {
          numLeadingDigits++;
        }
      }

      if (numLeadingDigits) {
        std::string leadingIntValue = qualityString.substr(0, numLeadingDigits);

        // Check to see if any of the valid options start with this option,
        // but make sure we don't select only the prefix of a number
        // (i.e.: if someone gives us "32", don't select "320 kbps")
        for (int i = 0; i < possibleQualityOptions.size(); i++) {
          const juce::String &option = possibleQualityOptions[i];
          if (option.startsWith(leadingIntValue) &&
              option.length() > leadingIntValue.size() &&
              !juce::CharacterFunctions::isDigit(
                  option[leadingIntValue.size()])) {
            qualityOptionIndex = i;
            break;
          }
        }
      } else {
        // If our search string doesn't start with leading digits,
        // check for a substring:
        for (int i = 0; i < possibleQualityOptions.size(); i++) {
          if (possibleQualityOptions[i].containsIgnoreCase(qualityString)) {
            qualityOptionIndex = i;
            break;
          }
        }
      }
    }

    // If we get here, we received a string we were unable to parse,
    // so the user should probably know about it:
    if (qualityOptionIndex == -1) {
      throw std::domain_error(
          "Unable to parse provided quality value (" + qualityString +
          "). Valid values for " + format->getFormatName().toStdString() +
          "s are: " +
          possibleQualityOptions.joinIntoString(", ").toStdString());
    }
  }

  if (qualityOptionIndex == -1) {
    if (possibleQualityOptions.size()) {
      // Choose the best quality by default if possible:
      qualityOptionIndex = possibleQualityOptions.size() - 1;
    } else {
      qualityOptionIndex = 0;
    }
  }

  return qualityOptionIndex;
}

class WriteableAudioFile
    : public AudioFile,
      public std::enable_shared_from_this<WriteableAudioFile> {
public:
  WriteableAudioFile(
      std::string filename, double writeSampleRate, int numChannels = 1,
      int bitDepth = 16,
      std::optional<std::variant<std::string, float>> qualityInput = {})
      : filename(filename) {
    pybind11::gil_scoped_release release;

    if (!isInteger(writeSampleRate)) {
      throw std::domain_error(
          "Opening an audio file for writing requires an integer sample rate.");
    }

    if (writeSampleRate == 0) {
      throw std::domain_error(
          "Opening an audio file for writing requires a non-zero sample rate.");
    }

    if (numChannels == 0) {
      throw py::type_error("Opening an audio file for writing requires a "
                           "non-zero num_channels.");
    }

    formatManager.registerBasicFormats();
    juce::File file(filename);

    std::unique_ptr<juce::FileOutputStream> outputStream =
        std::make_unique<juce::FileOutputStream>(file);
    if (!outputStream->openedOk()) {
      throw std::domain_error("Unable to open audio file for writing: " +
                              filename);
    }

    outputStream->setPosition(0);
    outputStream->truncate();

    juce::AudioFormat *format =
        formatManager.findFormatForFileExtension(file.getFileExtension());

    if (!format) {
      if (file.getFileExtension().isEmpty()) {
        throw std::domain_error("No file extension provided - cannot detect "
                                "audio format to write with for file path: " +
                                filename);
      }

      throw std::domain_error(
          "Unable to detect audio format for file extension: " +
          file.getFileExtension().toStdString());
    }

    // Normalize the input to a string here, as we need to do parsing anyways:
    std::string qualityString;
    if (qualityInput) {
      if (auto *q = std::get_if<std::string>(&qualityInput.value())) {
        qualityString = *q;
      } else if (auto *q = std::get_if<float>(&qualityInput.value())) {
        if (isInteger(*q)) {
          qualityString = std::to_string((int)*q);
        } else {
          qualityString = std::to_string(*q);
        }
      } else {
        throw std::runtime_error("Unknown quality type!");
      }
    }

    int qualityOptionIndex = determineQualityOptionIndex(format, qualityString);
    if (format->getQualityOptions().size() > qualityOptionIndex) {
      quality = format->getQualityOptions()[qualityOptionIndex].toStdString();
    }

    juce::StringPairArray emptyMetadata;
    writer.reset(format->createWriterFor(outputStream.get(), writeSampleRate,
                                         numChannels, bitDepth, emptyMetadata,
                                         qualityOptionIndex));
    if (!writer) {
      // Check common errors first:
      juce::Array<int> possibleSampleRates = format->getPossibleSampleRates();

      if (possibleSampleRates.isEmpty()) {
        throw std::domain_error(
            file.getFileExtension().toStdString() +
            " audio files are not writable with Pedalboard.");
      }

      if (!possibleSampleRates.contains((int)writeSampleRate)) {
        std::ostringstream sampleRateString;
        for (int i = 0; i < possibleSampleRates.size(); i++) {
          sampleRateString << possibleSampleRates[i];
          if (i < possibleSampleRates.size() - 1)
            sampleRateString << ", ";
        }
        throw std::domain_error(
            format->getFormatName().toStdString() +
            " audio files do not support the provided sample rate of " +
            std::to_string(writeSampleRate) +
            "Hz. Supported sample rates: " + sampleRateString.str());
      }

      juce::Array<int> possibleBitDepths = format->getPossibleBitDepths();

      if (possibleBitDepths.isEmpty()) {
        throw std::domain_error(
            file.getFileExtension().toStdString() +
            " audio files are not writable with Pedalboard.");
      }

      if (!possibleBitDepths.contains((int)bitDepth)) {
        std::ostringstream bitDepthString;
        for (int i = 0; i < possibleBitDepths.size(); i++) {
          bitDepthString << possibleBitDepths[i];
          if (i < possibleBitDepths.size() - 1)
            bitDepthString << ", ";
        }
        throw std::domain_error(
            format->getFormatName().toStdString() +
            " audio files do not support the provided bit depth of " +
            std::to_string(bitDepth) +
            " bits. Supported bit depths: " + bitDepthString.str());
      }

      std::string humanReadableQuality;
      if (qualityString.empty()) {
        humanReadableQuality = "None";
      } else {
        humanReadableQuality = qualityString;
      }

      throw std::domain_error(
          "Unable to create audio file writer with samplerate=" +
          std::to_string(writeSampleRate) +
          ", num_channels=" + std::to_string(numChannels) + ", bit_depth=" +
          std::to_string(bitDepth) + ", and quality=" + humanReadableQuality);
    } else {
      outputStream.release();
    }
  }

  template <typename SampleType>
  void write(py::array_t<SampleType, py::array::c_style> inputArray) {
    const juce::ScopedLock scopedLock(objectLock);

    if (!writer)
      throw std::runtime_error("I/O operation on a closed file.");

    py::buffer_info inputInfo = inputArray.request();

    unsigned int numChannels = 0;
    unsigned int numSamples = 0;
    ChannelLayout inputChannelLayout = detectChannelLayout(inputArray);

    // Release the GIL when we do the writing, after we
    // already have a reference to the input array:
    pybind11::gil_scoped_release release;

    if (inputInfo.ndim == 1) {
      numSamples = inputInfo.shape[0];
      numChannels = 1;
    } else if (inputInfo.ndim == 2) {
      // Try to auto-detect the channel layout from the shape
      if (inputInfo.shape[0] == getNumChannels() &&
          inputInfo.shape[1] == getNumChannels()) {
        throw std::runtime_error(
            "Unable to determine shape of audio input! Both dimensions have "
            "the same shape. Expected " +
            std::to_string(getNumChannels()) +
            "-channel audio, with one dimension larger than the other.");
      } else if (inputInfo.shape[1] == getNumChannels()) {
        numSamples = inputInfo.shape[0];
        numChannels = inputInfo.shape[1];
      } else if (inputInfo.shape[0] == getNumChannels()) {
        numSamples = inputInfo.shape[1];
        numChannels = inputInfo.shape[0];
      } else {
        throw std::runtime_error(
            "Unable to determine shape of audio input! Expected " +
            std::to_string(getNumChannels()) + "-channel audio.");
      }
    } else {
      throw std::runtime_error(
          "Number of input dimensions must be 1 or 2 (got " +
          std::to_string(inputInfo.ndim) + ").");
    }

    if (numChannels == 0) {
      // No work to do.
      return;
    } else if (numChannels != getNumChannels()) {
      throw std::runtime_error(
          "WritableAudioFile was opened with num_channels=" +
          std::to_string(getNumChannels()) +
          ", but was passed an array containing " +
          std::to_string(numChannels) + "-channel audio!");
    }

    // Depending on the input channel layout, we need to copy data
    // differently. This loop is duplicated here to move the if statement
    // outside of the tight loop, as we don't need to re-check that the input
    // channel is still the same on every iteration of the loop.
    switch (inputChannelLayout) {
    case ChannelLayout::Interleaved: {
      std::vector<std::vector<SampleType>> deinterleaveBuffers;

      // Use a temporary buffer to chunk the audio input
      // and pass it into the writer, chunk by chunk, rather
      // than de-interleaving the entire buffer at once:
      deinterleaveBuffers.resize(numChannels);

      const SampleType **channelPointers =
          (const SampleType **)alloca(numChannels * sizeof(SampleType *));
      for (int startSample = 0; startSample < numSamples;
           startSample += DEFAULT_AUDIO_BUFFER_SIZE_FRAMES) {
        int samplesToWrite = std::min(numSamples - startSample,
                                      DEFAULT_AUDIO_BUFFER_SIZE_FRAMES);

        for (int c = 0; c < numChannels; c++) {
          deinterleaveBuffers[c].resize(samplesToWrite);
          channelPointers[c] = deinterleaveBuffers[c].data();

          // We're de-interleaving the data here, so we can't use copyFrom.
          for (unsigned int i = 0; i < samplesToWrite; i++) {
            deinterleaveBuffers[c][i] =
                ((SampleType
                      *)(inputInfo.ptr))[((i + startSample) * numChannels) + c];
          }
        }

        if (!write(channelPointers, numChannels, samplesToWrite)) {
          throw std::runtime_error("Unable to write data to audio file.");
        }
      }

      break;
    }
    case ChannelLayout::NotInterleaved: {
      // We can just pass all the data to write:
      const SampleType **channelPointers =
          (const SampleType **)alloca(numChannels * sizeof(SampleType *));
      for (int c = 0; c < numChannels; c++) {
        channelPointers[c] = ((SampleType *)inputInfo.ptr) + (numSamples * c);
      }
      if (!write(channelPointers, numChannels, numSamples)) {
        throw std::runtime_error("Unable to write data to audio file.");
      }
      break;
    }
    default:
      throw std::runtime_error(
          "Internal error: got unexpected channel layout.");
    }

    framesWritten += numSamples;
  }

  template <typename TargetType, typename InputType,
            unsigned int bufferSize = DEFAULT_AUDIO_BUFFER_SIZE_FRAMES>
  bool writeConvertingTo(const InputType **channels, int numChannels,
                         unsigned int numSamples) {
    std::vector<std::vector<TargetType>> targetTypeBuffers;
    targetTypeBuffers.resize(numChannels);

    const TargetType **channelPointers =
        (const TargetType **)alloca(numChannels * sizeof(TargetType *));
    for (unsigned int startSample = 0; startSample < numSamples;
         startSample += bufferSize) {
      int samplesToWrite = std::min(numSamples - startSample, bufferSize);

      for (int c = 0; c < numChannels; c++) {
        targetTypeBuffers[c].resize(samplesToWrite);
        channelPointers[c] = targetTypeBuffers[c].data();

        if constexpr (std::is_integral<InputType>::value) {
          if constexpr (std::is_integral<TargetType>::value) {
            for (unsigned int i = 0; i < samplesToWrite; i++) {
              // Left-align the samples to use all 32 bits, as JUCE requires:
              targetTypeBuffers[c][i] =
                  ((int)channels[c][startSample + i])
                  << (std::numeric_limits<int>::digits -
                      std::numeric_limits<InputType>::digits);
            }
          } else if constexpr (std::is_same<TargetType, float>::value) {
            constexpr auto scaleFactor =
                1.0f / static_cast<float>(std::numeric_limits<int>::max());
            juce::FloatVectorOperations::convertFixedToFloat(
                targetTypeBuffers[c].data(), channels[c] + startSample,
                scaleFactor, samplesToWrite);
          } else {
            // We should never get here - this would only be true
            // if converting to double, which no formats require:
            static_assert(std::is_integral<InputType>::value &&
                              std::is_same<TargetType, double>::value,
                          "Can't convert to double");
          }
        } else {
          if constexpr (std::is_integral<TargetType>::value) {
            // We should never get here - this would only be true
            // if converting float to int, which JUCE handles for us:
            static_assert(std::is_integral<TargetType>::value &&
                              !std::is_integral<InputType>::value,
                          "Can't convert float to int");
          } else {
            // Converting double to float:
            for (unsigned int i = 0; i < samplesToWrite; i++) {
              targetTypeBuffers[c][i] = channels[c][startSample + i];
            }
          }
        }
      }

      if (!write(channelPointers, numChannels, samplesToWrite)) {
        return false;
      }
    }

    return true;
  }

  template <typename SampleType>
  bool write(const SampleType **channels, int numChannels,
             unsigned int numSamples) {
    if constexpr (std::is_integral<SampleType>::value) {
      if constexpr (std::is_same<SampleType, int>::value) {
        if (writer->isFloatingPoint()) {
          return writeConvertingTo<float>(channels, numChannels, numSamples);
        } else {
          return writer->write(channels, numSamples);
        }
      } else {
        return writeConvertingTo<int>(channels, numChannels, numSamples);
      }
    } else if constexpr (std::is_same<SampleType, float>::value) {
      if (writer->isFloatingPoint()) {
        // Just pass the floating point data into the writer as if it were
        // integer data. If the writer requires floating-point input data, this
        // works (and is documented!)
        return writer->write((const int **)channels, numSamples);
      } else {
        // Convert floating-point to fixed point, but let JUCE do that for us:
        return writer->writeFromFloatArrays(channels, numChannels, numSamples);
      }
    } else {
      // We must have double-format data:
      return writeConvertingTo<float>(channels, numChannels, numSamples);
    }
  }

  void flush() {
    if (!writer)
      throw std::runtime_error("I/O operation on a closed file.");
    const juce::ScopedLock scopedLock(objectLock);
    pybind11::gil_scoped_release release;

    if (!writer->flush()) {
      throw std::runtime_error(
          "Unable to flush audio file; is the underlying file seekable?");
    }
  }

  void close() {
    if (!writer)
      throw std::runtime_error("Cannot close closed file.");
    const juce::ScopedLock scopedLock(objectLock);
    writer.reset();
  }

  bool isClosed() const {
    const juce::ScopedLock scopedLock(objectLock);
    return !writer;
  }

  double getSampleRate() const {
    if (!writer)
      throw std::runtime_error("I/O operation on a closed file.");
    return writer->getSampleRate();
  }

  std::string getFilename() const { return filename; }

  long getFramesWritten() const { return framesWritten; }

  std::optional<std::string> getQuality() const { return quality; }

  long getNumChannels() const {
    if (!writer)
      throw std::runtime_error("I/O operation on a closed file.");
    return writer->getNumChannels();
  }

  std::string getFileDatatype() const {
    if (!writer)
      throw std::runtime_error("I/O operation on a closed file.");

    if (writer->isFloatingPoint()) {
      switch (writer->getBitsPerSample()) {
      case 16: // OGG returns 16-bit int data, but internally stores floats
      case 32:
        return "float32";
      case 64:
        return "float64";
      default:
        return "unknown";
      }
    } else {
      switch (writer->getBitsPerSample()) {
      case 8:
        return "int8";
      case 16:
        return "int16";
      case 24:
        return "int24";
      case 32:
        return "int32";
      case 64:
        return "int64";
      default:
        return "unknown";
      }
    }
  }

  std::shared_ptr<WriteableAudioFile> enter() { return shared_from_this(); }

  void exit(const py::object &type, const py::object &value,
            const py::object &traceback) {
    close();
  }

private:
  juce::AudioFormatManager formatManager;
  std::string filename;
  std::optional<std::string> quality;
  std::unique_ptr<juce::AudioFormatWriter> writer;
  juce::CriticalSection objectLock;
  int framesWritten = 0;
};

inline void init_audiofile(py::module &m) {
  py::class_<AudioFile, std::shared_ptr<AudioFile>>(
      m, "AudioFile", "A base class for readable and writeable audio files.")
      .def(py::init<>()) // Make this class effectively abstract; we can only
                         // instantiate subclasses.
      .def_static(
          "__new__",
          [](const py::object *, std::string filename, std::string mode) {
            if (mode == "r") {
              return std::make_shared<ReadableAudioFile>(filename);
            } else if (mode == "w") {
              throw py::type_error("Opening an audio file for writing requires "
                                   "samplerate and num_channels arguments.");
            } else {
              throw py::type_error("AudioFile instances can only be opened in "
                                   "read mode (\"r\") and write mode (\"w\").");
            }
          },
          py::arg("cls"), py::arg("filename"), py::arg("mode") = "r")
      .def_static(
          "__new__",
          [](const py::object *, std::string filename, std::string mode,
             std::optional<double> sampleRate, int numChannels, int bitDepth,
             std::optional<std::variant<std::string, float>> quality) {
            if (mode == "r") {
              throw py::type_error(
                  "Opening an audio file for reading does not require "
                  "samplerate, num_channels, bit_depth, or quality arguments - "
                  "these parameters "
                  "will be read from the file.");
            } else if (mode == "w") {
              if (!sampleRate) {
                throw py::type_error(
                    "Opening an audio file for writing requires a samplerate "
                    "argument to be provided.");
              }

              return std::make_shared<WriteableAudioFile>(
                  filename, sampleRate.value(), numChannels, bitDepth, quality);
            } else {
              throw py::type_error("AudioFile instances can only be opened in "
                                   "read mode (\"r\") and write mode (\"w\").");
            }
          },
          py::arg("cls"), py::arg("filename"), py::arg("mode") = "w",
          py::arg("samplerate") = py::none(), py::arg("num_channels") = 1,
          py::arg("bit_depth") = 16, py::arg("quality") = py::none());

  py::class_<ReadableAudioFile, AudioFile, std::shared_ptr<ReadableAudioFile>>(
      m, "ReadableAudioFile",
      "An audio file reader interface, with native support for Ogg Vorbis, "
      "MP3, WAV, FLAC, and AIFF files on all operating systems. On some "
      "platforms, other formats may also be readable. (Use "
      "pedalboard.io.get_supported_read_formats() to see which formats are "
      "supported on the current platform.)")
      .def(py::init([](std::string filename) {
             return std::make_shared<ReadableAudioFile>(filename);
           }),
           py::arg("filename"))
      .def_static(
          "__new__",
          [](const py::object *, std::string filename) {
            return std::make_shared<ReadableAudioFile>(filename);
          },
          py::arg("cls"), py::arg("filename"))
      .def(
          "read", &ReadableAudioFile::read, py::arg("num_frames") = 0,
          "Read the given number of frames (samples in each channel) from this "
          "audio file at the current position. Audio samples are returned in "
          "the shape (channels, samples); i.e.: a stereo audio file will have "
          "shape (2, <length>). Returned data is always in float32 format.")
      .def(
          "read_raw", &ReadableAudioFile::readRaw, py::arg("num_frames") = 0,
          "Read the given number of frames (samples in each channel) from this "
          "audio file at the current position. Audio samples are returned in "
          "the shape (channels, samples); i.e.: a stereo audio file will have "
          "shape (2, <length>). Returned data is in the raw format stored by "
          "the underlying file (one of int8, int16, int32, or float32).")
      .def("seekable", &ReadableAudioFile::isSeekable,
           "Returns True if this file is currently open and calls to seek() "
           "will work.")
      .def("seek", &ReadableAudioFile::seek, py::arg("position"),
           "Seek this file to the provided location in frames.")
      .def("tell", &ReadableAudioFile::tell,
           "Fetch the position in this audio file, in frames.")
      .def("close", &ReadableAudioFile::close,
           "Close this file, rendering this object unusable.")
      .def("__enter__", &ReadableAudioFile::enter)
      .def("__exit__", &ReadableAudioFile::exit)
      .def("__repr__",
           [](const ReadableAudioFile &file) {
             std::ostringstream ss;
             ss << "<pedalboard.io.ReadableAudioFile";
             if (!file.getFilename().empty()) {
               ss << " filename=\"" << file.getFilename() << "\"";
             }
             if (file.isClosed()) {
               ss << " closed";
             } else {
               ss << " samplerate=" << file.getSampleRate();
               ss << " num_channels=" << file.getNumChannels();
               ss << " frames=" << file.getLengthInSamples();
               ss << " file_dtype=" << file.getFileDatatype();
             }
             ss << " at " << &file;
             ss << ">";
             return ss.str();
           })
      .def_property_readonly("name", &ReadableAudioFile::getFilename,
                             "The name of this file.")
      .def_property_readonly(
          "closed", &ReadableAudioFile::isClosed,
          "If this file has been closed, this property will be True.")
      .def_property_readonly("samplerate", &ReadableAudioFile::getSampleRate,
                             "The sample rate of this file in samples "
                             "(per channel) per second (Hz).")
      .def_property_readonly("channels", &ReadableAudioFile::getNumChannels,
                             "The number of channels in this file.")
      .def_property_readonly("frames", &ReadableAudioFile::getLengthInSamples,
                             "The total number of frames (samples per "
                             "channel) in this file.")
      .def_property_readonly(
          "duration", &ReadableAudioFile::getDuration,
          "The duration of this file (frames divided by sample rate).")
      .def_property_readonly(
          "file_dtype", &ReadableAudioFile::getFileDatatype,
          "The data type stored natively by this file. Note that read(...) "
          "will always return a float32 array, regardless of the value of this "
          "property.");

  py::class_<WriteableAudioFile, AudioFile,
             std::shared_ptr<WriteableAudioFile>>(
      m, "WriteableAudioFile",
      "An audio file writer interface, with native support for Ogg Vorbis, "
      "MP3, WAV, FLAC, and AIFF files on all operating systems. (Use "
      "pedalboard.io.get_supported_write_formats() to see which formats are "
      "supported on the current platform.)")
      .def(
          py::init([](std::string filename, double sampleRate, int numChannels,
                      int bitDepth,
                      std::optional<std::variant<std::string, float>> quality) {
            return std::make_shared<WriteableAudioFile>(
                filename, sampleRate, numChannels, bitDepth, quality);
          }),
          py::arg("filename"), py::arg("samplerate"),
          py::arg("num_channels") = 1, py::arg("bit_depth") = 16,
          py::arg("quality") = py::none())
      .def_static(
          "__new__",
          [](const py::object *, std::string filename,
             std::optional<double> sampleRate, int numChannels, int bitDepth,
             std::optional<std::variant<std::string, float>> quality) {
            if (!sampleRate) {
              throw py::type_error(
                  "Opening an audio file for writing requires a samplerate "
                  "argument to be provided.");
            }
            return std::make_shared<WriteableAudioFile>(
                filename, sampleRate.value(), numChannels, bitDepth, quality);
          },
          py::arg("cls"), py::arg("filename"),
          py::arg("samplerate") = py::none(), py::arg("num_channels") = 1,
          py::arg("bit_depth") = 16, py::arg("quality") = py::none())
      .def(
          "write",
          [](WriteableAudioFile &file, py::array_t<char> samples) {
            file.write<char>(samples);
          },
          py::arg("samples").noconvert(),
          "Encode an array of int8 (8-bit signed integer) audio data and write "
          "it to this file. The number of channels in the array must match the "
          "number of channels used to open the file. The array may contain "
          "audio in any shape. If the file's bit depth or format does not "
          "match this data type, the audio will be automatically converted.")
      .def(
          "write",
          [](WriteableAudioFile &file, py::array_t<short> samples) {
            file.write<short>(samples);
          },
          py::arg("samples").noconvert(),
          "Encode an array of int16 (16-bit signed integer) audio data and "
          "write it to this file. The number of channels in the array must "
          "match the number of channels used to open the file. The array may "
          "contain audio in any shape. If the file's bit depth or format does "
          "not match this data type, the audio will be automatically "
          "converted.")
      .def(
          "write",
          [](WriteableAudioFile &file, py::array_t<int> samples) {
            file.write<int>(samples);
          },
          py::arg("samples").noconvert(),
          "Encode an array of int32 (32-bit signed integer) audio data and "
          "write it to this file. The number of channels in the array must "
          "match the number of channels used to open the file. The array may "
          "contain audio in any shape. If the file's bit depth or format does "
          "not match this data type, the audio will be automatically "
          "converted.")
      .def(
          "write",
          [](WriteableAudioFile &file, py::array_t<float> samples) {
            file.write<float>(samples);
          },
          py::arg("samples").noconvert(),
          "Encode an array of float32 (32-bit floating-point) audio data and "
          "write it to this file. The number of channels in the array must "
          "match the number of channels used to open the file. The array may "
          "contain audio in any shape. If the file's bit depth or format does "
          "not match this data type, the audio will be automatically "
          "converted.")
      .def(
          "write",
          [](WriteableAudioFile &file, py::array_t<double> samples) {
            file.write<double>(samples);
          },
          py::arg("samples").noconvert(),
          "Encode an array of float64 (64-bit floating-point) audio data and "
          "write it to this file. The number of channels in the array must "
          "match the number of channels used to open the file. The array may "
          "contain audio in any shape. No supported formats support float64 "
          "natively, so the audio will be converted automatically.")
      .def("flush", &WriteableAudioFile::flush,
           "Attempt to flush this audio file's contents to disk. Not all "
           "formats support flushing, so this may throw a RuntimeError. (If "
           "this happens, closing the file will reliably force a flush to "
           "occur.)")
      .def("close", &WriteableAudioFile::close,
           "Close this file, flushing its contents to disk and rendering this "
           "object unusable for further writing.")
      .def("__enter__", &WriteableAudioFile::enter)
      .def("__exit__", &WriteableAudioFile::exit)
      .def("__repr__",
           [](const WriteableAudioFile &file) {
             std::ostringstream ss;
             ss << "<pedalboard.io.WriteableAudioFile";
             if (!file.getFilename().empty()) {
               ss << " filename=\"" << file.getFilename() << "\"";
             }
             if (file.isClosed()) {
               ss << " closed";
             } else {
               ss << " samplerate=" << file.getSampleRate();
               ss << " num_channels=" << file.getNumChannels();
               if (file.getQuality()) {
                 ss << " quality=\"" << file.getQuality().value() << "\"";
               }
               ss << " file_dtype=" << file.getFileDatatype();
             }
             ss << " at " << &file;
             ss << ">";
             return ss.str();
           })
      .def_property_readonly(
          "closed", &WriteableAudioFile::isClosed,
          "If this file has been closed, this property will be True.")
      .def_property_readonly("samplerate", &WriteableAudioFile::getSampleRate,
                             "The sample rate of this file in samples "
                             "(per channel) per second (Hz).")
      .def_property_readonly("channels", &WriteableAudioFile::getNumChannels,
                             "The number of channels in this file.")
      .def_property_readonly("frames", &WriteableAudioFile::getFramesWritten,
                             "The total number of frames (samples per "
                             "channel) written to this file so far.")
      .def_property_readonly(
          "file_dtype", &WriteableAudioFile::getFileDatatype,
          "The data type stored natively by this file. Note that write(...) "
          "will accept multiple datatypes, regardless of the value of this "
          "property.")
      .def_property_readonly(
          "quality", &WriteableAudioFile::getQuality,
          "The quality setting used to write this file. For many "
          "formats, this may be None.");

  m.def("get_supported_read_formats", []() {
    juce::AudioFormatManager manager;
    manager.registerBasicFormats();

    std::vector<std::string> formatNames(manager.getNumKnownFormats());
    juce::StringArray extensions;
    for (int i = 0; i < manager.getNumKnownFormats(); i++) {
      auto *format = manager.getKnownFormat(i);
      extensions.addArray(format->getFileExtensions());
    }

    extensions.trim();
    extensions.removeEmptyStrings();
    extensions.removeDuplicates(true);

    std::vector<std::string> output;
    for (juce::String s : extensions) {
      output.push_back(s.toStdString());
    }

    std::sort(
        output.begin(), output.end(),
        [](const std::string lhs, const std::string rhs) { return lhs < rhs; });

    return output;
  });

  m.def("get_supported_write_formats", []() {
    // JUCE doesn't support writing other formats out-of-the-box on all
    // platforms, and there's no easy way to tell which formats are supported
    // without attempting to create an AudioFileWriter object - so this list is
    // hardcoded for now.
    const std::vector<std::string> formats = {".aiff", ".flac", ".ogg", ".wav"};
    return formats;
  });
}
} // namespace Pedalboard