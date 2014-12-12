/*
    Copyright Dan Petro, 2014

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include <stdio.h>
#include <getopt.h>
#include <execinfo.h>
#include <signal.h>
#include <chrono>
#include <atomic>
#include <thread>

#include "ConsoleColors.h"
#include "Untwister.h"

using std::chrono::seconds;
using std::chrono::milliseconds;
using std::chrono::duration;
using std::chrono::duration_cast;
using std::chrono::steady_clock;

static const unsigned int ONE_YEAR = 31536000;
static const unsigned int TRACE_SIZE = 10;

/* Segfault handler - for debugging only */
void handler(int sig) {
    void *trace[TRACE_SIZE];
    size_t size = backtrace(trace, TRACE_SIZE);
    std::cerr << "[!] SIGSEGV: " << sig << std::endl;
    backtrace_symbols_fd(trace, size, 2);
    exit(1);
}

void Usage(Untwister *untwister)
{
    std::cout << BOLD << "Untwister" << RESET << " - Recover PRNG seeds from observed values." << std::endl;
    std::cout << "\t-i <input_file> [-d <depth> ] [-r <prng>] [-g <seed>] [-t <threads>] [-c <confidence>]\n" << std::endl;
    std::cout << "\t-i <input_file>\n\t\tPath to file input file containing observed results of your RNG. The contents" << std::endl;
    std::cout << "\t\tare expected to be newline separated 32-bit integers. See test_input.txt for" << std::endl;
    std::cout << "\t\tan example." << std::endl;
    std::cout << "\t-d <depth>\n\t\tThe depth (default 1000) to inspect for each seed value when brute forcing." << std::endl;
    std::cout << "\t\tChoosing a higher depth value will make brute forcing take longer (linearly), but is" << std::endl;
    std::cout << "\t\trequired for cases where the generator has been used many times already." << std::endl;
    std::cout << "\t-r <prng>\n\t\tThe PRNG algorithm to use. Supported PRNG algorithms:" << std::endl;
    std::vector<std::string> names = untwister->getPRNGNames();
    for (unsigned int index = 0; index < names.size(); ++index)
    {
        std::cout << "\t\t" << BOLD << " * " << RESET << names[index];
        if (index == 0)
        {
            std::cout << " (default)";
        }
        std::cout << std::endl;
    }
    std::cout << "\t-u\n\t\tUse bruteforce, but only for unix timestamp values within a range of +/- 1 " << std::endl;
    std::cout << "\t\tyear from the current time." << std::endl;
    std::cout << "\t-g <seed>\n\t\tGenerate a test set of random numbers from the given seed (at a random depth)" << std::endl;
    std::cout << "\t-c <confidence>\n\t\tSet the minimum confidence percentage to report" << std::endl;
    std::cout << "\t-t <threads>\n\t\tSpawn this many threads (default is " << untwister->getThreads() << ")" << std::endl;
    std::cout << std::endl;
}


void DisplayProgress(Untwister *untwister, uint32_t totalWork)
{
    std::atomic<bool>* isRunning = untwister->getIsRunning();
    while (!isRunning->load(std::memory_order_relaxed))
    {
        std::this_thread::sleep_for(milliseconds(100));
    }

    double percent = 0.0;
    double seedsPerSec = 0.0;
    double timeLeft = 0.0;
    steady_clock::time_point started = steady_clock::now();
    std::vector<uint32_t> *status = untwister->getStatus();
    std::atomic<bool> *isCompleted = untwister->getIsCompleted();
    char spinner[] = {'|', '/', '-', '\\'};
    unsigned int count = 0;
    while (!isCompleted->load(std::memory_order_relaxed))
    {
        unsigned int sum = 0;
        duration<double> time_span = duration_cast<duration<double>>(steady_clock::now() - started);
        for (unsigned int index = 0; index < status->size(); ++index)
        {
            sum += status->at(index);
        }
        percent = ((double) sum / (double) totalWork) * 100.0;
        if (0 < time_span.count())
        {
            seedsPerSec = (double) sum / (double) time_span.count();
            if (0 == count % 20)
            {
                timeLeft = ((double) (totalWork - sum) / seedsPerSec) / 60.0;
            }
        }

        std::cout << CLEAR << BOLD << PURPLE << "[" << spinner[count % 4] << "]" << RESET
                  << " Progress: " << percent << '%'
                  << "  [" << sum << " / " << totalWork << "]"
                  << "  ~" << seedsPerSec << "/sec"
                  << "  " << timeLeft << " minute(s)";
        std::cout.flush();
        ++count;
        std::this_thread::sleep_for(milliseconds(100));
    }
    std::cout << CLEAR;
}

void FindSeed(Untwister *untwister, uint32_t lowerBoundSeed, uint32_t upperBoundSeed)
{
    std::cout << INFO << "Looking for seed using " << BOLD << untwister->getPRNG() << RESET << std::endl;
    std::cout << INFO << "Spawning " << untwister->getThreads() << " worker thread(s) ..." << std::endl;

    steady_clock::time_point elapsed = steady_clock::now();
    std::thread progressThread(DisplayProgress, untwister, upperBoundSeed - lowerBoundSeed);

    auto results = untwister->bruteforce(lowerBoundSeed, upperBoundSeed);
    auto isCompleted = untwister->getIsCompleted();
    if (!isCompleted->load(std::memory_order_relaxed))
    {
        isCompleted->store(true, std::memory_order_relaxed);
    }
    progressThread.join();

    /* Total time elapsed */
    std::cout << INFO << "Completed in "
              << duration_cast<seconds>(steady_clock::now() - elapsed).count()
              << " second(s)" << std::endl;

    /* Display results */
    for (unsigned int index = 0; index < results.size(); ++index)
    {
        std::cout << SUCCESS << "Found seed " << results[index].first
                  << " with a confidence of " << results[index].second
                  << '%' << std::endl;
    }
}

int main(int argc, char *argv[])
{
    /* Signal Handlers */
    signal(SIGSEGV, handler);
    signal(SIGILL, handler);
    signal(SIGABRT, handler);

    int c;

    uint32_t lowerBoundSeed = 0;
    uint32_t upperBoundSeed = UINT_MAX;
    uint32_t seed = 0;
    bool generateFlag = false;
    Untwister *untwister = new Untwister();

    while ((c = getopt(argc, argv, "d:i:g:t:r:c:uh")) != -1)
    {
        switch (c)
        {
            case 'g':
            {
                seed = strtoul(optarg, NULL, 10);
                generateFlag = true;
                break;
            }
            case 'u':
            {
                lowerBoundSeed = time(NULL) - ONE_YEAR;
                upperBoundSeed = time(NULL) + ONE_YEAR;
                break;
            }
            case 'r':
            {
                if (!untwister->isSupportedPRNG(optarg))
                {
                    std::cerr << WARN << "ERROR: The PRNG \"" << optarg << "\" is not supported, see -h" << std::endl;
                    return EXIT_FAILURE;
                }
                else
                {
                    untwister->setPRNG(optarg);
                }
                break;
            }
            case 'd':
            {
                unsigned int depth = strtoul(optarg, NULL, 10);
                if (depth == 0)
                {
                    std::cerr << WARN << "ERROR: Please enter a valid depth > 1" << std::endl;
                    return EXIT_FAILURE;
                }
                else
                {
                    std::cout << INFO << "Depth set to: " << depth << std::endl;
                    untwister->setDepth(depth);
                }
                break;
            }
            case 'i':
            {
                std::ifstream infile(optarg);
                if (!infile)
                {
                    std::cerr << WARN << "ERROR: File \"" << optarg << "\" not found" << std::endl;
                }
                std::string line;
                while (std::getline(infile, line))
                {
                    uint32_t value = strtoul(line.c_str(), NULL, 0);
                    untwister->addObservedOutput(value);
                }
                break;
            }
            case 't':
            {
                unsigned int threads = strtoul(optarg, NULL, 10);
                if (threads == 0)
                {
                    std::cerr << WARN << "ERROR: Please enter a valid number of threads > 1" << std::endl;
                    return EXIT_FAILURE;
                }
                else
                {
                    untwister->setThreads(threads);
                }
                break;
            }
            case 'c':
            {
                double minimumConfidence = ::atof(optarg);
                if (minimumConfidence <= 0 || 100.0 < minimumConfidence)
                {
                    std::cerr << WARN << "ERROR: Invalid confidence percentage " << std::endl;
                    return EXIT_FAILURE;
                }
                else
                {
                    std::cout << INFO << "Minimum confidence set to: " << minimumConfidence << std::endl;
                    untwister->setMinConfidence(minimumConfidence);
                }
                break;
            }
            case 'h':
            {
                Usage(untwister);
                return EXIT_SUCCESS;
            }
            case '?':
            {
                if (optopt == 'd')
                   std::cerr << "Option -" << optopt << " requires an argument." << std::endl;
                else if (isprint(optopt))
                   std::cerr << "Unknown option `-" << optopt << "'." << std::endl;
                else
                   std::cerr << "Unknown option character `" << optopt << "'." << std::endl;
                Usage(untwister);
                return EXIT_FAILURE;
            }
            default:
            {
                Usage(untwister);
                return EXIT_FAILURE;
            }
        }
    }

    if (generateFlag)
    {
        std::vector<uint32_t> results;
        if(untwister->getObservedOutputs()->empty())
        {
            results = untwister->generateSampleFromSeed(seed);
        }
        else
        {
            results = untwister->generateSampleFromState();
        }
        for (unsigned int index = 0; index < results.size(); ++index)
        {
            std::cout << results.at(index) << std::endl;
        }
        return EXIT_SUCCESS;

    }

    if (untwister->getObservedOutputs()->empty())
    {
        Usage(untwister);
        std::cerr << WARN << "ERROR: No input numbers provided. Use -i <file> to provide a file" << std::endl;
        return EXIT_FAILURE;
    }

    if (untwister->inferState())
    {
        return EXIT_SUCCESS;
    }

    FindSeed(untwister, lowerBoundSeed, upperBoundSeed);
    delete untwister;

    return EXIT_SUCCESS;
}
