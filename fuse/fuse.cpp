/* Wake FUSE launcher to capture inputs/outputs
 *
 * Copyright 2019 SiFive, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You should have received a copy of LICENSE.Apache2 along with
 * this software. If not, you may obtain a copy at
 *
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <iostream>
#include <fstream>
#include <sstream>

#include "json5.h"
#include "execpath.h"
#include "membytes.h"

#ifdef __linux__
#include "namespace.h"
#endif

int run_in_fuse(
	const std::string& daemon_path,
	const std::string& working_dir,
	const std::string& json,
	std::string& result_json)
{
	if (0 != chdir(working_dir.c_str())) {
		std::cerr << "chdir " << working_dir << ": " << strerror(errno) << std::endl;
		return 1;
	}

	JAST jast;
	if (!JAST::parse(json, std::cerr, jast))
		return 1;

	std::string name  = std::to_string(getpid());
	// mpath is where the fuse filesystem is mounted
	std::string mpath = working_dir + "/.fuse";
	std::string fpath = mpath + "/.f.fuse-waked";
	// rpath is a subdir in the fuse filesystem that will be used by this fuse-wake
	std::string rpath = mpath + "/" + name;
	// Lock file held by each child of fuse-wake. When all children close it,
	// the daemon releases the resources for that job
	std::string lpath = mpath + "/.l." + name;
	// Input (as json) to the fuse daemon to setup visible files.
	std::string ipath = mpath + "/.i." + name;
	// Result metadata from the daemon
	std::string opath = mpath + "/.o." + name;

	int ffd = -1;
	useconds_t wait = 10000; /* 10ms */
	for (int retry = 0; (ffd = open(fpath.c_str(), O_RDONLY)) == -1 && retry < 12; ++retry) {
		pid_t pid = fork();
		if (pid == 0) {
			const char *env[2] = { "PATH=/usr/bin:/bin:/usr/sbin:/sbin", 0 };
			execle(daemon_path.c_str(), "fuse-waked", mpath.c_str(), nullptr, env);
			std::cerr << "execl " << daemon_path << ": " << strerror(errno) << std::endl;
			exit(1);
		}
		usleep(wait);
		wait <<= 1;

		int status;
		do waitpid(pid, &status, 0);
		while (WIFSTOPPED(status));
	}

	if (ffd == -1) {
		std::cerr << "Could not contact FUSE daemon" << std::endl;
		return 1;
	}

	// This stays open (keeping rpath live) until we terminate
	// Note: O_CLOEXEC is NOT set; thus, children keep rpath live as well
	int livefd = open(lpath.c_str(), O_CREAT|O_RDWR|O_EXCL, 0644);
	if (livefd == -1) {
		std::cerr << "open " << rpath << ": " << strerror(errno) << std::endl;
		return 1;
	}

	// We can safely release the global handle now that we hold a livefd
	(void)close(ffd);

	std::ofstream ijson(ipath);
	ijson.write(json.data(), json.size());
	if (ijson.fail()) {
		std::cerr << "write " << ipath << ": " << strerror(errno) << std::endl;
		return 1;
	}
	ijson.close();

	struct timeval start;
	gettimeofday(&start, 0);

	pid_t pid = fork();
	if (pid == 0) {
		// Kernel to send signal to child process if parent dies
		if (prctl(PR_SET_PDEATHSIG, SIGTERM) == -1) {
			std::cerr << "prctl: " << strerror(errno) << std::endl;
			exit(1);
		}

		// Become a target for group death
		// setpgid(0, 0);

		// Prepare the subcommand inputs
		std::vector<char *> arg, env;
		for (auto &x : jast.get("command").children)
			arg.push_back(const_cast<char*>(x.second.value.c_str()));
		arg.push_back(0);
		for (auto &x : jast.get("environment").children)
			env.push_back(const_cast<char*>(x.second.value.c_str()));
		env.push_back(0);

		std::string subdir = jast.get("directory").value;
		std::string stdin = jast.get("stdin").value;
		if (stdin.empty()) stdin = "/dev/null";

#ifdef __linux__

		if (!setup_user_namespaces(jast))
			exit(1);

		// Mounts from the parsed input json.
		if (!do_mounts_from_json(jast, rpath))
			exit(1);

		std::string dir;
		if (!get_workspace_dir(jast, working_dir, dir)) {
			std::cerr << "'workspace' mount entry is missing from input" << std::endl;
			exit(1);
		}
		dir = dir + "/" + subdir;
#else
		std::string dir = rpath + "/" + subdir;
#endif

		if (chdir(dir.c_str()) != 0) {
			std::cerr << "chdir " << dir << ": " << strerror(errno) << std::endl;
			exit(1);
		}

		int fd = open(stdin.c_str(), O_RDONLY);
		if (fd == -1) {
			std::cerr << "open " << stdin << ":" << strerror(errno) << std::endl;
			exit(1);
		}

		if (fd != STDIN_FILENO) {
			dup2(fd, STDIN_FILENO);
			close(fd);
		}

		std::string command = find_in_path(arg[0], find_path(env.data()));
		execve(command.c_str(), arg.data(), env.data());
		std::cerr << "execve " << command << ": " << strerror(errno) << std::endl;
		exit(1);
	}
	jast.children.clear();

	// Don't hold IO open while waiting
	(void)close(STDIN_FILENO);
	(void)close(STDOUT_FILENO);
	(void)close(STDERR_FILENO);

	int status;
	struct rusage rusage;
	do wait4(pid, &status, 0, &rusage);
	while (WIFSTOPPED(status));

	if (WIFEXITED(status)) {
		status = WEXITSTATUS(status);
	} else {
		status = -WTERMSIG(status);
	}

	struct timeval stop;
	gettimeofday(&stop, 0);

	// Cause the opath to be generated (this write will fail)
	(void)!write(livefd, &stop, 1); // the ! convinces older gcc that it's ok to ignore the write
	(void)fsync(livefd);

	std::stringstream ss;
	if (!JAST::parse(opath.c_str(), ss, jast)) {
		// stderr is closed, so report the error on the only output we have
		result_json = ss.str();
		return 1;
	}

	bool first;
	ss << "{\"usage\":{\"status\":" << status
	  << ",\"runtime\":" << (stop.tv_sec - start.tv_sec + (stop.tv_usec - start.tv_usec)/1000000.0)
	  << ",\"cputime\":" << (rusage.ru_utime.tv_sec + rusage.ru_stime.tv_sec + (rusage.ru_utime.tv_usec + rusage.ru_stime.tv_usec)/1000000.0)
	  << ",\"membytes\":" << MEMBYTES(rusage)
	  << ",\"inbytes\":" << jast.get("ibytes").value
	  << ",\"outbytes\":" << jast.get("obytes").value
	  << "},\"inputs\":[";

	first = true;
	for (auto &x : jast.get("inputs").children) {
		ss << (first?"":",") << "\"" << json_escape(x.second.value) << "\"";
		first = false;
	}

	ss << "],\"outputs\":[";

	first = true;
	for (auto &x : jast.get("outputs").children) {
		ss << (first?"":",") << "\"" << json_escape(x.second.value) << "\"";
		first = false;
	}

	ss << "]}" << std::endl;
	result_json = ss.str();

	return ss.fail() ? 1 : 0;
}
