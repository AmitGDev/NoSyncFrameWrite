/*
	NoSyncFrameWrite
	Copyright (c) 2024, Amit Gefen

	Permission is hereby granted, free of charge, to any person obtaining a copy
	of this software and associated documentation files (the "Software"), to deal
	in the Software without restriction, including without limitation the rights
	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
	copies of the Software, and to permit persons to whom the Software is
	furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in
	all copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
	THE SOFTWARE.
*/

#include <memory>
#include <cstring>
#include <span>
#include <algorithm>
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cmath>
#include <format>
#include <array>
#include <ranges>


namespace // (Anonymous namespace)
{

	// Utility__

	// Get the current time.
	static auto Now()
	{
		return std::chrono::steady_clock::now();
	}


	// Print the duration since a given start time.
	static void PrintDuration(const std::chrono::time_point<std::chrono::steady_clock> start_time)
	{
		const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Now() - start_time);
		std::cout << "(execution time: " << duration_ms.count() << " milliseconds)" << std::endl;
	}


	// Format a large character count with appropriate units (K, M, G, etc.)
	std::string FormatCharCount(uint64_t char_count)
	{
		// Array of units:
		const std::array<const char*, 9> units = { "", "K", "M", "G", "T", "P", "E", "Z", "Y" }; // (index 0-8)

		// Calculate the appropriate unit index based on the number of characters:
		// (std::min ensures the calculated unit index doesn't exceed the bounds of the units array.)
		const int unit_index = std::min(static_cast<int>(std::log2(char_count) / 10), 8);

		// Format the number with the appropriate unit:
		return unit_index == 0 ? std::format("{}", char_count) : std::format("{}{}", char_count / std::pow(1024, unit_index), units[unit_index]);
	}

	// __Utility


	//	Frame class: Represents a rectangular frame of characters.
	//
	//	buffer_ (std::unique_ptr<char[]>)                                	<-- Pointer to dynamically allocated memory.
	//	+-------------------------------+-------------------------------|   
	//	|                               |                               |	<-- Rows (size_t). See: GetRows().
	//	+-------------------------------+-------------------------------|
	//	|                               |                               |	<-- Cols (size_t). See: GetCols().
	//	+-------------------------------+-------------------------------|
	//	|       |       |       |       |       |       |       |       |	<-- Frame data (characters). See: GetDataIndex()
	//	+-------+-------+-------+-------+-------+-------+-------+-------|
	//	| ...   | ...   | ...   | ...   | ...   | ...   | ...   | ...   |
	//	+-------+-------+-------+-------+-------+-------+-------+-------|
	//
	//	buffer_: Pointer to a dynamically allocated memory block holding the frame data.
	//	- The first `sizeof(size_t)` bytes (typically 8 bytes on modern systems) store the number of rows.
	//	- The next `sizeof(size_t)` bytes (typically 8 bytes on modern systems) store the number of columns.
	//	- The remaining memory stores the frame data, with each character occupying 1 byte.

	class Frame final
	{
	public:

		// Struct to define a rectangle within the frame:
		struct Rect final
		{
			size_t x1{ 0 }, y1{ 0 }, x2{ 0 }, y2{ 0 };
		};


		// Constructor to create a frame with given dimensions:
		Frame(const size_t rows, const size_t cols)
		{
			[[maybe_unused]] const auto create_ok{ Create(rows, cols) };
		}


		// Draw "White" if frame with optimized_n worker-threads.
		bool Draw(const Rect& rect, const size_t n = 1) const
		{
			const size_t cols_to_draw{ (rect.y2 - rect.y1) + 1 };

			// Dynamic thread count optimization:
			const size_t optimized_n = std::clamp(n, static_cast<size_t>(1), // Min.
				std::min(cols_to_draw, static_cast<size_t>(std::thread::hardware_concurrency()))); // Max.

			std::string note{ "main-thread" };
			if (optimized_n > 1) {
				note = "threads: " + std::to_string(optimized_n - 1) + " worker threads + " + note;
			}

			const auto chars{ FormatCharCount((rect.x2 - rect.x1 + 1) * (rect.y2 - rect.y1 + 1)) };
			std::cout << "draw (" << note << ") (x1-y1: " << rect.x1 << "-" << rect.y1 << ", x2-y2: " << rect.x2 << "-" << rect.y2 << ", total: " << chars << " chars)" << std::endl;

			if (!DrawSanityChecks(rect)) {
				std::cerr << "error: Draw() sanity check failed." << std::endl;

				return false;
			}

			const auto start_time = Now(); // <-- Start.

			if (optimized_n > 1) { // Run with worker-threads:
				std::vector<std::pair<size_t, size_t>> segments(optimized_n);
				PrepareSegments(cols_to_draw, segments);

				std::vector<std::jthread> threads;
				threads.reserve(optimized_n - 1);

				// (optimized_n - 1) worker threads:
				for (unsigned int i = 0; i < optimized_n - 1; ++i) {
					threads.emplace_back([&, i]() { DrawThread(rect, segments[i]); }); // This uses the default capture mode (&), capturing all variables by reference, except i, which is captured by value.
				}

				// + [main thread]:
				DrawThread(rect, segments[optimized_n - 1]);

				for (auto& thread : threads) {
					thread.join();
				}
			}
			else { // Run with main-thread:
				DrawThread(rect, { 0, rect.y2 - rect.y1 });
			}

			PrintDuration(start_time); // <-- Finish.

			return true;
		}


		// Print the frame.
		// This is mainly for debug / demo.
		// Usefull on small frame (~ up to 100 rows).
		[[nodiscard]] bool PrintFrame() const
		{
			if (buffer_ == nullptr) { // Create() failed.
				std::cerr << "error: PrintFrame() frame buffer is nullptr." << std::endl;

				return false;
			}

			size_t start_p{ GetDataIndex() };

			std::cout << "frame" << std::endl;
			for (size_t i = 0; i < GetCols(); ++i) {
				std::span<char> char_span(&buffer_.get()[start_p], GetRows());
				std::ranges::for_each(char_span, [](char c) { std::cout << ((c == 0) ? '0' : '1'); }); std::cout << std::endl;
				start_p += GetRows();
			}

			return true;
		}

	protected:

		// Assign a "segment" of cols_to_draw for each thread (so we don't need thread syncronization): 
		void PrepareSegments(const size_t cols_to_draw, std::vector<std::pair<size_t, size_t>>& segments) const
		{
			// Calculate the size of each segment to evenly distribute the drawing workload among them.
			// Calculate the remainder to handle cases where the number of columns to draw is not evenly divisible by the number of chunks.
			const size_t segment_size{ cols_to_draw / segments.size() },
				remainder = cols_to_draw % segments.size();

			// Set the size of each segment based on the calculated segment size and distribute the remaining workload.
			for (int i = 0; i < segments.size(); ++i) {
				segments[i].first = segment_size;
				if (i < remainder) {
					++segments[i].first;
				}
			}

			// Adjust the 'from' and 'to' indices of each segment to properly define their boundaries.
			size_t temp{ 0 };
			for (int i = 0; i < segments.size(); ++i) {
				segments[i].second = (segments[i].first - 1) + temp;
				segments[i].first = temp;
				temp = segments[i].second + 1;
			}

			// Print segment loads:
			int i{ 1 };
			for (const auto& segment : segments) {
				std::cout << "* thread " << i++ << ": col " << segment.first << " - " << segment.second << std::endl;
			}
		}


		// Check that Draw() is feasible.
		[[nodiscard]] bool DrawSanityChecks(const Rect& rect) const
		{
			return (!(rect.x1 < 0 || rect.y1 < 0 // x1-y1 negative.
				|| rect.x2 < rect.x1 || rect.y2 < rect.y1 // x1-y1 is beyond x2-y2.
				|| rect.x2 > GetRows() - 1 || rect.y2 > GetCols() - 1 // x2-y2 is exceeds frame.
				|| buffer_ == nullptr)); // Create() failed.
		}


		// (Run in the context of multiple threads; no syncronization! - SEGMENTS SHOULD NOT OVERLAP!)
		// Draw segment. 
		// segment is col from - to offsets (*relative to rect.y1*).
		void DrawThread(const Rect& rect, std::pair<size_t, size_t> segment) const
		{
			const auto id{ std::this_thread::get_id() };

			for (size_t i = segment.first; i <= segment.second; ++i) {

				// (GetDataIndex() is added only later. For easier envision while dev.)
				size_t start_p{ (rect.x1 + (i + rect.y1) * GetRows()) }; // if y1 == 0, juts x1.
				const size_t size_for_memset{ (rect.x2 - rect.x1) + 1 }; // if x2 == x1, 1 char.

				start_p += GetDataIndex();
 
				// (Each thread writes to an exclusive segments of the buffer => No need for mutex.)
				std::span<char> char_span(&buffer_.get()[start_p], size_for_memset);
				std::ranges::fill(char_span, 0x00); // Draw "White" (0x00).
			}
		}


		// Create a blank frame.
		[[nodiscard]] bool Create(const size_t rows, const size_t cols)
		{
			// If rows and/or cols 0, return false. buffer_ stays nullptr.
			if (!(rows > 0 && cols > 0)) {
				std::cerr << "error: Create() rows and/or cols 0." << std::endl;

				return false;
			}

			try
			{
				const size_t buffer_size{ GetDataIndex() + (cols * rows) };
				buffer_ = std::unique_ptr<char[]>(new char[buffer_size]); // std::make_unique<char[]>(buffer_size); (As of C++20, std::make_unique does not support dynamic arrays)

				// Embeds rows and cols into buffer_:
				std::memcpy(buffer_.get(), &rows, sizeof(size_t));
				std::memcpy(buffer_.get() + sizeof(size_t), &cols, sizeof(size_t));

				// Initialize empty frame:
				std::memset(buffer_.get() + GetDataIndex(), 0xFF, cols * rows); // Draw "Black" (0xFF).

				std::cout << "create frame (rows: " << rows << ", cols: " << cols << ")" << std::endl;
			}
			catch ([[maybe_unused]] const std::bad_alloc& e)
			{
				// buffer_ stays nullptr.
				return false;
			}

			return true;
		}


		// Buffer indicators__

		// Extract rows from the buffer.
		[[nodiscard]] size_t GetRows() const
		{
			if (buffer_ == nullptr) { // Create() failed.
				std::cerr << "error: GetRows() frame buffer is nullptr." << std::endl;

				return 0;
			}

			size_t rows{ 0 };
			std::memcpy(&rows, buffer_.get(), sizeof(size_t));

			return rows;
		}


		// Extract cols from the buffer .
		[[nodiscard]] size_t GetCols() const
		{
			if (buffer_ == nullptr) { // Create() failed.
				std::cerr << "error: GetCols() frame buffer is nullptr." << std::endl;

				return 0;
			}

			size_t cols{ 0 };
			std::memcpy(&cols, buffer_.get() + sizeof(size_t), sizeof(size_t));

			return cols;
		}


		// Return the index where the frame begins.
		[[nodiscard]] constexpr size_t GetDataIndex() const // The result is computed during compilation.
		{
			return sizeof(size_t) * 2;
		}

		// __Buffer indicators


		std::unique_ptr<char[]> buffer_{}; // [rows][cols][....frame data....]
	};


	// Let's visually confirm that it is functioning correctly. 
	static void TestFunctionality()
	{
		std::cout << "**** test functionality: small frame + small draw + print frame: ****\n" << std::endl;

		std::vector<std::pair<size_t, size_t>> xy_sets{ { 10, 15 } /* Possible to add here more frames */ };
		for (const auto& xy : xy_sets)
		{
			Frame frame{ xy.first, xy.second };
			std::cout << std::endl;

			[[maybe_unused]] bool ok{ frame.Draw({ 1, 1, 3, 2 }) };
			std::cout << std::endl;

			ok = frame.PrintFrame();
			std::cout << std::endl;

			ok = frame.Draw({ 5, 1, 8, 13 }, 2);
			std::cout << std::endl;

			ok = frame.PrintFrame();
			std::cout << std::endl;
		}
	}


	// Let's assess the performance on a very large frame with a different thread count.
	static void TestPerformance()
	{
		std::cout << "**** test performance (hardware concurrency: " << std::thread::hardware_concurrency() << "): large frame + large draw: ****\n" << std::endl;

		constexpr size_t kFrameRows = 600000;
		constexpr size_t kFrameCols = 2000;

		// 512MB
		constexpr size_t kDrawRows = 524288; // 512KB
		constexpr size_t kDrawCols = 1024; // 1KB

		Frame frame{ kFrameRows, kFrameCols };
		std::cout << std::endl;

		{
			size_t buffer_size{ static_cast<size_t>(kDrawRows) * kDrawCols }; // Cast sub-expr. to a wider type.

			std::cout << "benchmark: std::memcpy of " << buffer_size << " bytes" << std::endl;

			auto start_time = Now(); // <-- Start.

			// Benchmark:
			auto buffer = std::unique_ptr<char[]>(new char[buffer_size]);
			std::memset(buffer.get(), '0', buffer_size);

			PrintDuration(start_time); // <-- Finish.
			std::cout << std::endl;
		}

		[[maybe_unused]] bool ok = frame.Draw({ 1, 1, kDrawRows, kDrawCols }, 1);
		std::cout << std::endl;

		ok = frame.Draw({ 1, 1, kDrawRows, kDrawCols }, 2);
		std::cout << std::endl;

		ok = frame.Draw({ 1, 1, kDrawRows, kDrawCols }, 4);
		std::cout << std::endl;

		ok = frame.Draw({ 1, 1, kDrawRows, kDrawCols }, 8);
		std::cout << std::endl;

		ok = frame.Draw({ 1, 1, kDrawRows, kDrawCols }, 12);
		std::cout << std::endl;
	}

} // (Anonymous namespace)


int main()
{
	TestFunctionality();
	std::cout << std::endl;
	TestPerformance();
}