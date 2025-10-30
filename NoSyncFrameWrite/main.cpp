/*
    NoSyncFrameWrite/main.cpp
    Copyright (c) 2024-2025, Amit Gefen

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
    deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <memory>
#include <new>
#include <ranges>  // NOLINT(misc-include-cleaner)
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace  // (Anonymous namespace)
{

constexpr double kBase1024 = 1024.0;
constexpr unsigned char kColorBlack = 0xFF;
constexpr unsigned char kColorWhite = 0x00;

// Utility__

// Get the current time.
auto Now() { return std::chrono::steady_clock::now(); }

// Print the duration since a given start time.
void PrintDuration(
    const std::chrono::time_point<std::chrono::steady_clock> start_time) {
  const auto duration_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(Now() - start_time);
  std::cout << "(execution time: " << duration_ms.count() << " milliseconds)"
            << '\n';
}

// Format a large character count with appropriate units (K, M, G, etc.)
std::string FormatCharCount(const uint64_t char_count) {
  constexpr int kMaxUnitIndex = 8;
  static constexpr std::array<const char*, kMaxUnitIndex + 1> kUnits = {
      "", "K", "M", "G", "T", "P", "E", "Z", "Y"};

  // Calculate the appropriate unit index based on the number of characters:
  // (std::min ensures the calculated unit index doesn't exceed the bounds of
  // the units array.)
  const int unit_index =
      std::min(static_cast<int>(std::log2(char_count) / 10), kMaxUnitIndex);

  // Format the number with the appropriate unit:
  return unit_index == 0 ? std::format("{}", char_count)
                         : std::format("{}{}",
                                       static_cast<double>(char_count) /
                                           std::pow(kBase1024, unit_index),
                                       kUnits.at(unit_index));
}

// __Utility

//	Frame class: Represents a rectangular frame of characters.
//
//	buffer_ (std::unique_ptr<char[]>)                                  <-- Pointer to dynamically allocated memory.
//	+-------------------------------+-------------------------------|
//	|                               |                               |  <-- Rows (size_t). See: GetRows().
//	+-------------------------------+-------------------------------|
//	|                               |                               |  <-- Cols (size_t). See: GetCols().
//	+-------------------------------+-------------------------------|
//	|       |       |       |       |       |       |       |       |  <-- Frame data (characters). See: GetDataIndex()
//	+-------+-------+-------+-------+-------+-------+-------+-------|
//	| ...   | ...   | ...   | ...   | ...   | ...   | ...   | ...   |
//	+-------+-------+-------+-------+-------+-------+-------+-------|
//
//	buffer_: Pointer to a dynamically allocated memory block holding the frame data.
//	- The first `sizeof(size_t)` bytes (typically 8 bytes on modern systems) store the number of rows.
//	- The next `sizeof(size_t)` bytes (typically 8 bytes on modern systems) store the number of columns.
//	- The remaining memory stores the frame data, with each character ccupying 1 byte.

class Frame final {
 public:
  // Struct to define a rectangle within the frame:
  struct Rect final {
    size_t x1{0}, y1{0}, x2{0}, y2{0};
  };

  // Constructor to create a frame with given dimensions:
  Frame(const size_t rows, const size_t cols) {
    [[maybe_unused]] const auto create_ok{Create(rows, cols)};
  }

  // Draw "White" if frame with optimized_n worker-threads.
  [[nodiscard]] bool Draw(const Rect& rect,
                          const size_t thread_count = 1) const {
    const size_t cols_to_draw{(rect.y2 - rect.y1) + 1};

    // Dynamic thread count optimization:
    const size_t optimized_thread_count = std::clamp(
        thread_count, static_cast<size_t>(1),  // Min.
        std::min(
            cols_to_draw,
            static_cast<size_t>(std::thread::hardware_concurrency())));  // Max.

    std::string note{"main-thread"};
    if (optimized_thread_count > 1) {
      note = "threads: " + std::to_string(optimized_thread_count - 1) +
             " worker threads + " + note;
    }

    const auto chars{
        FormatCharCount((rect.x2 - rect.x1 + 1) * (rect.y2 - rect.y1 + 1))};
    std::cout << "draw (" << note << ") (x1-y1: " << rect.x1 << "-" << rect.y1
              << ", x2-y2: " << rect.x2 << "-" << rect.y2
              << ", total: " << chars << " chars)" << '\n';

    if (!DrawSanityChecks(rect)) {
      std::cerr << "error: Draw() sanity check failed." << '\n';

      return false;
    }

    const auto start_time = Now();  // <-- Start.

    if (optimized_thread_count > 1) {  // Run with worker-threads:
      std::vector<std::pair<size_t, size_t>> segments(optimized_thread_count);
      PrepareSegments(cols_to_draw, segments);

      std::vector<std::jthread> threads;
      threads.reserve(optimized_thread_count - 1);

      // (optimized_thread_count - 1) worker threads:
      for (unsigned int i = 0; i < optimized_thread_count - 1; ++i) {
        threads.emplace_back([&, i]() {
          DrawThread(rect, segments[i]);
        });  // This uses the default capture mode (&), capturing all variables
             // by reference, except i, which is captured by value.
      }

      // + [main thread]:
      DrawThread(rect, segments[optimized_thread_count - 1]);

      for (auto& thread : threads) {
        thread.join();
      }
    } else {  // Run with main-thread:
      DrawThread(rect, {0, rect.y2 - rect.y1});
    }

    PrintDuration(start_time);  // <-- Finish.

    return true;
  }

  // Print the frame.
  // This is mainly for debug / demo.
  // Usefull on small frame (~ up to 100 rows).
  [[nodiscard]] bool PrintFrame() const {
    if (buffer_ == nullptr) {  // Create() failed.
      std::cerr << "error: PrintFrame() frame buffer is nullptr." << '\n';

      return false;
    }

    size_t start_p{GetDataIndex()};

    std::cout << "frame" << '\n';
    for (size_t index = 0; index < GetCols(); ++index) {
      const std::span character_span(&buffer_.get()[start_p], GetRows());
      std::ranges::for_each(character_span, [](char character) {
        std::cout << ((character == 0) ? '0' : '1');
      });
      std::cout << '\n';
      start_p += GetRows();
    }

    return true;
  }

 protected:
  // Assign a "segment" of cols_to_draw for each thread (so we don't need thread
  // syncronization):
  static void PrepareSegments(
      const size_t cols_to_draw,
      std::vector<std::pair<size_t, size_t>>& segments) {
    // Calculate the size of each segment to evenly distribute the drawing
    // workload among them. Calculate the remainder to handle cases where the
    // number of columns to draw is not evenly divisible by the number of
    // chunks.
    const size_t segment_size{cols_to_draw / segments.size()};
    const size_t remainder = cols_to_draw % segments.size();

    // Set the size of each segment based on the calculated segment size and
    // distribute the remaining workload.
    for (int index = 0; index < segments.size(); ++index) {
      segments[index].first = segment_size;
      if (index < remainder) {
        ++segments[index].first;
      }
    }

    // Adjust the 'from' and 'to' indices of each segment to properly define
    // their boundaries.
    size_t temp{0};
    for (auto& segment : segments) {
      segment.second = (segment.first - 1) + temp;
      segment.first = temp;
      temp = segment.second + 1;
    }

    // Print segment loads:
    // Create a 1-based index view and zip it with segments
    for (const auto& [index, segment] :
         std::views::zip(std::views::iota(1), segments)) {
      std::cout << "* thread " << index << ": col " << segment.first << " - "
                << segment.second << '\n';
    }
  }

  // Check that Draw() is feasible.
  [[nodiscard]] bool DrawSanityChecks(const Rect& rect) const {
    return (rect.x1 >= 0 && rect.y1 >= 0  // x1-y1 negative.
            && rect.x2 >= rect.x1 &&
            rect.y2 >= rect.y1  // x1-y1 is beyond x2-y2.
            && rect.x2 <= GetRows() - 1 &&
            rect.y2 <= GetCols() - 1  // x2-y2 is exceeds frame.
            && buffer_ != nullptr);   // Create() failed.
  }

  // (Run in the context of multiple threads; no syncronization! - SEGMENTS
  // SHOULD NOT OVERLAP!) Draw segment. segment is col from - to offsets
  // (*relative to rect.y1*).
  void DrawThread(const Rect& rect,
                  const std::pair<size_t, size_t> segment) const {
    for (size_t index = segment.first; index <= segment.second; ++index) {
      // (GetDataIndex() is added only later. For easier envision while dev.)
      size_t start_p{
          (rect.x1 + ((index + rect.y1) * GetRows()))};  // if y1 == 0, juts x1.
      const size_t size_for_memset{(rect.x2 - rect.x1) +
                                   1};  // if x2 == x1, 1 char.

      start_p += GetDataIndex();

      // (Each thread writes to an exclusive segments of the buffer => No need
      // for mutex.)
      const std::span<char> char_span(&buffer_.get()[start_p], size_for_memset);
      std::ranges::fill(char_span, kColorWhite);  // Draw "White" (kColorWhite).
    }
  }

  // Create a blank frame.
  [[nodiscard]] bool Create(const size_t rows, const size_t cols) {
    // If rows and/or cols 0, return false. buffer_ stays nullptr.
    if (rows <= 0 || cols <= 0) {
      std::cerr << "error: Create() rows and/or cols 0." << '\n';

      return false;
    }

    try {
      const size_t buffer_size{GetDataIndex() + (cols * rows)};
      buffer_ = std::unique_ptr<char[]>(
          new char[buffer_size]);  // std::make_unique<char[]>(buffer_size); (As
                                   // of C++20, std::make_unique does not
                                   // support dynamic arrays)

      // Embeds rows and cols into buffer_:
      std::memcpy(buffer_.get(), &rows, sizeof(size_t));
      std::memcpy(buffer_.get() + sizeof(size_t), &cols, sizeof(size_t));

      // Initialize empty frame:
      std::memset(buffer_.get() + GetDataIndex(), kColorBlack,
                  cols * rows);  // Draw "Black" (kColorBlack).

      std::cout << "create frame (rows: " << rows << ", cols: " << cols << ")"
                << '\n';
    } catch ([[maybe_unused]] const std::bad_alloc& e) {
      // buffer_ stays nullptr.
      return false;
    }

    return true;
  }

  // Buffer indicators__

  // Extract rows from the buffer.
  [[nodiscard]] size_t GetRows() const {
    if (buffer_ == nullptr) {  // Create() failed.
      std::cerr << "error: GetRows() frame buffer is nullptr." << '\n';

      return 0;
    }

    size_t rows{0};
    std::memcpy(&rows, buffer_.get(), sizeof(size_t));

    return rows;
  }

  // Extract cols from the buffer .
  [[nodiscard]] size_t GetCols() const {
    if (buffer_ == nullptr) {  // Create() failed.
      std::cerr << "error: GetCols() frame buffer is nullptr." << '\n';

      return 0;
    }

    size_t cols{0};
    std::memcpy(&cols, buffer_.get() + sizeof(size_t), sizeof(size_t));

    return cols;
  }

  // Return the index where the frame begins.
  [[nodiscard]] static constexpr size_t
  GetDataIndex()  // The result is computed during compilation.
  {
    return sizeof(size_t) * 2;
  }

  // __Buffer indicators

  std::unique_ptr<char[]> buffer_;  // [rows][cols][....frame data....]
};

// Let's visually confirm that it is functioning correctly.
void TestFunctionality() {
  std::cout << "**** test functionality: small frame + small draw + print "
               "frame: ****\n"
            << '\n';
  ;

  // XY frame sizes.
  const std::vector<std::pair<size_t, size_t>> xy_frames{
      {10, 15} /* Possible to add here more xy_frames */};

  // Rects to draw inside each xy_frame.
  const auto rects = std::to_array<Frame::Rect>({{1, 1, 3, 2}, {5, 1, 8, 13}});

  for (const auto& xy_frame : xy_frames) {  // For each frame size
    // Create frame
    const Frame frame{xy_frame.first, xy_frame.second};  // Create
    // Draw rects and print frame
    for (const auto& rect : rects) {  // For each rect
      std::cout << '\n';
      [[maybe_unused]] const auto result1 = frame.Draw(rect, 2); // Draw
      std::cout << '\n';
      [[maybe_unused]] const auto result2 = frame.PrintFrame();  // Print
      std::cout << '\n';
    }
  }
}

// Let's assess the performance on a very large frame with a different thread
// count.
void TestPerformance() {
  std::cout << "**** test performance (hardware concurrency: "
            << std::thread::hardware_concurrency()
            << "): large frame + large draw: ****\n"
            << '\n';

  constexpr size_t kFrameRows = 600000;
  constexpr size_t kFrameCols = 2000;

  // 512MB frame draw:
  constexpr size_t kDrawRows = 524288;  // 512KB
  constexpr size_t kDrawCols = 1024;    // 1KB

  constexpr std::array<uint8_t, 5> kThreadCounts = {1, 2, 4, 8, 12};

  const Frame frame{kFrameRows, kFrameCols};
  std::cout << '\n';

  {
    const size_t buffer_size{static_cast<size_t>(kDrawRows) *
                             kDrawCols};  // Cast sub-expr. to a wider type.

    std::cout << "benchmark: std::memcpy of " << buffer_size << " bytes"
              << '\n';

    const auto start_time = Now();  // <-- Start.

    // Benchmark:
    auto buffer = std::unique_ptr<char[]>(new char[buffer_size]);
    std::memset(buffer.get(), '0', buffer_size);

    PrintDuration(start_time);  // <-- Finish.
    std::cout << '\n';
  }

  for (const auto thread_count : kThreadCounts) {
    [[maybe_unused]] const bool done =
        frame.Draw({1, 1, kDrawRows, kDrawCols}, thread_count);
    std::cout << '\n';
  }
}

}  // namespace

int main() {  // NOLINT(bugprone-exception-escape)
  TestFunctionality();
  std::cout << '\n';
  TestPerformance();
}