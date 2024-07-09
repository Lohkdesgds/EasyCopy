#include <fstream>
#include <filesystem>
#include <string>
#include <memory>
#include <future>
#include <thread>
#include <Lunaris/console/console.h>
#include <Lunaris/event_pool/event_pool.h>

const unsigned g_thread_copy_count = std::thread::hardware_concurrency() * 100; // up to 1600 copies per run on my PC
const unsigned g_max_queued = std::thread::hardware_concurrency() * 250; // up to 4000 queued on my pc

using namespace Lunaris;

class custom_fstream : std::fstream {
	const bool enabled;
public:
	custom_fstream(const bool cancel) : std::fstream(), enabled(false) {}
	custom_fstream(const std::string& path) : std::fstream(path, std::ios::binary | std::ios::out), enabled(this->good() && this->is_open()) {}
	custom_fstream(custom_fstream&& m) noexcept : std::fstream(std::move(m)), enabled(m.enabled) {}
	void operator<<(const std::string& t) { if (enabled) this->std::fstream::write(t.c_str(), t.size()); }

	bool valid() const { return enabled; }
};

class custom_string : std::string {
public:
	custom_string(const std::string& str) : std::string(str) {}
	bool operator&(const char& has) const { return this->find(has) != std::string::npos; }
};

// Remove / at the end and make all /
std::string filter_folder(std::string beg) {
	std::for_each(beg.begin(), beg.end(), [](char& ch) { if (ch == '\\') ch = '/'; });
	while (beg.size() && beg.back() == '/') beg.pop_back();
	return beg;
}

int main(int argc, char* argv[])
{
	if (argc < 3) {
		Lunaris::cout << console::color::YELLOW << "Not enough arguments.";
		Lunaris::cout << "Try app.exe <from folder> <to folder> [<extra params>]";
		Lunaris::cout << "Params (concatenate them):";
		Lunaris::cout << "V: Verbose (show every copy / mkdir)";
		//Lunaris::cout << "S: Write file sizes on log";
		return 1;
	}

	const std::string				a_from = filter_folder(argv[1]);
	const std::string				a_targ = filter_folder(argv[2]);
	const custom_string				a_params(argc > 3 ? argv[3] : "");
	//custom_fstream					log = argc > 3 ? custom_fstream(std::string(argv[3])) : custom_fstream(false);

	std::atomic_size_t made_count = 0, read_count = 0, err_count = 0;
	event_pool_async<std::function<void()>> event_pool([](auto&& f) { if (f) { f(); } }, g_thread_copy_count);

	Lunaris::cout << console::color::GRAY << "Processing copy of folder " << console::color::GREEN << a_from << console::color::GRAY << " to " << console::color::LIGHT_PURPLE << a_targ << console::color::GRAY;
	//Lunaris::cout << console::color::GRAY << "Logging enabled / good? " << (log.valid() ? "YES" : "no");

	namespace fs = std::filesystem;

	const auto fs_from = fs::recursive_directory_iterator{ a_from };

	Lunaris::cout << console::color::GREEN << "Generated recursive directory object. Working on it...";

	const auto run_time_started = std::chrono::high_resolution_clock::now();


	auto async_list = std::async(std::launch::async, [&]() -> bool {

		Lunaris::cout << console::color::DARK_GREEN << "Async load of items began...";

		for (const auto& each : fs_from) {

			if (event_pool.size_queued() > g_max_queued) {
				Lunaris::cout << console::color::DARK_GREEN << "Async load stopped because queue is high. Waiting for queue to go down to continue searching for files to queue...";
				while (event_pool.size_queued() > (g_max_queued / 2)) std::this_thread::sleep_for(std::chrono::milliseconds(100));
				Lunaris::cout << console::color::DARK_GREEN << "Async load is back searching for new files to copy...";
			}

			std::string rel_path_log = each.path().string().substr(a_from.size());
			std::for_each(rel_path_log.begin(), rel_path_log.end(), [](char& ch) { if (ch == '\\') ch = '/'; });
			std::error_code err;

			if (each.is_directory()) {
				const std::string depth_str(fs_from.depth(), ' ');
				//log << depth_str + "FOLDER: " + rel_path_log + "\n";
				if (a_params & 'V') Lunaris::cout << console::color::DARK_GRAY << depth_str << "Folder -> /" << rel_path_log;
			
				fs::create_directories(a_targ + "/" + rel_path_log, err);

				if (err) {
					Lunaris::cout << console::color::RED << depth_str << "Error code #" << err.value() << " when creating folder " << rel_path_log << ". Details: " << err.message();
					//log << depth_str + "ERROR FOLDER CREATION #" + std::to_string(err.value()) + " [DETAIL: " + err.message() + "]: " + rel_path_log + "\n";
				}
			}
			else if (each.is_regular_file())
			{
				++read_count;
				event_pool.post([&fs_from, &a_params, &a_targ, &err_count, &made_count, rel_path_log, selfpath = each.path().string()] {
					const std::string depth_str(fs_from.depth(), ' ');

					//log << depth_str + "FILE: " + rel_path_log;
					if (a_params & 'V') { Lunaris::cout << console::color::DARK_GRAY << depth_str << "BEGIN: " << rel_path_log; }
					//std::this_thread::sleep_for(std::chrono::milliseconds(300));

					const auto time_start = std::chrono::high_resolution_clock::now();

					std::fstream i(selfpath, std::ios::binary | std::ios::in);

					const auto time_open_i = std::chrono::high_resolution_clock::now();
					if (time_open_i - time_start > std::chrono::seconds(10) && (a_params & 'V')) Lunaris::cout << console::color::GOLD << depth_str << "WARN: " << rel_path_log << " took some time to open input!";

					if (!i || i.bad() || !i.is_open()) {
						Lunaris::cout << console::color::RED << depth_str << "Cannot open file for reading: " << selfpath << ". Skipped.";
						//log << depth_str + "ERROR FILE READ OPEN: " + each.path().string() + "\n";
						++err_count;
						return;
					}

					std::fstream o(a_targ + "/" + rel_path_log, std::ios::binary | std::ios::out);

					const auto time_open_o = std::chrono::high_resolution_clock::now();
					if (time_open_o - time_open_i > std::chrono::seconds(10) && (a_params & 'V')) Lunaris::cout << console::color::GOLD << depth_str << "WARN: " << rel_path_log << " took some time to open output!";

					if (!o || o.bad() || !o.is_open()) {
						Lunaris::cout << console::color::RED << depth_str << "Cannot open file for writing: " << (a_targ + "/" + rel_path_log) << ". Skipped.";
						//log << depth_str + "ERROR FILE WRITE OPEN: " + a_targ + "/" + rel_path_log + "\n";
						++err_count;
						return;
					}

					i.seekg(0, std::ios::end);
					const auto total_len = i.tellg();
					i.seekg(0, std::ios::beg);
					//std::this_thread::sleep_for(std::chrono::milliseconds(200));

					if (a_params & 'V') { Lunaris::cout << console::color::DARK_PURPLE << depth_str << "WORK: " << rel_path_log << "; size=" << total_len << " byte(s)"; }

					//if (a_params & 'S') { log << " [SIZE: " + std::to_string(total_len) + " byte(s)]"; }
					//log << std::string("\n");

					const size_t len = 1 << 18;
					std::unique_ptr<char[]> blk = std::make_unique<char[]>(len); // 256 kb (big blocks!)

					//printf_s("00.000%% \r");

					while (!i.eof() && i.good() && o.good()) {
						i.read(blk.get(), len);
						const auto gottn = i.gcount();
						if (gottn == 0) continue;
						o.write(blk.get(), gottn);

						//printf_s("%02.3lf%% \r", (o.tellp() * 100.0 / (static_cast<double>(total_len) + 1)));
						//std::this_thread::sleep_for(std::chrono::milliseconds(50));
					}

					const auto time_flushed = std::chrono::high_resolution_clock::now();

					if (a_params & 'V') { Lunaris::cout << console::color::DARK_GREEN << depth_str << "ENDED: " << rel_path_log << "; size=" << total_len << " byte(s) [t: " << (std::chrono::duration_cast<std::chrono::milliseconds>(time_flushed - time_start).count() * 0.001) << " sec(s)]"; }
					++made_count;
				});			
			}
		}


		Lunaris::cout << console::color::DARK_GREEN << "Async load of items ended! All tasks queued!";
		return true;
	});

	Lunaris::cout << console::color::GREEN << "Queued all tasks...";

	do {
		const auto thrinf = event_pool.get_threads_status();

		double avg_latencies[2]{};
		size_t thr_in_use_count = 0;

		for (const auto& i : thrinf) {
			avg_latencies[0] += i.latency_get / 1e6;
			avg_latencies[1] += i.latency_run / 1e6;
			thr_in_use_count += i.is_tasking ? 1 : 0;
		}

		avg_latencies[0] *= 1.0 / thrinf.size();
		avg_latencies[1] *= 1.0 / thrinf.size();

		const std::string title = (std::to_string(static_cast<unsigned long>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now() - run_time_started).count())) +
			" sec | DONE;TOTAL;QUEUED;ERRORS: " + std::to_string(made_count) + ";" + std::to_string(read_count) + ";" + std::to_string(event_pool.size_queued()) + ";" + std::to_string(err_count) +
			" | POOL Load: " + std::to_string(thr_in_use_count * 100.0 / event_pool.thread_count()) + "% | Averages: WAIT;RUN (sec): " + std::to_string(avg_latencies[0]) + ";" + std::to_string(avg_latencies[1]));
#ifdef _WIN32
		SetConsoleTitleA(
			title.c_str()
		);
#else
		std::cout << "\033]0;" << title << "\007";
#endif

		std::this_thread::yield();
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	} while (event_pool.has_task_running() || async_list.wait_for(std::chrono::seconds(0)) != std::future_status::ready);

#ifdef _WIN32
	SetConsoleTitleA("Ended!");
#else
	std::cout << "\033]0;Ended!\007";
#endif

	Lunaris::cout << console::color::GREEN << "Ended. Have a nice day!";
	return 0;
}