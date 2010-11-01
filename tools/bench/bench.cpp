/*
 * bench.cpp --
 *
 * Copyright (c) 2007-2010, Dmitry Prokoptsev <dprokoptsev@gmail.com>,
 *                          Alexander Gololobov <agololobov@gmail.com>
 *
 * This file is part of Pire, the Perl Incompatible
 * Regular Expressions library.
 *
 * Pire is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * Pire is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser Public License for more details.
 * You should have received a copy of the GNU Lesser Public License
 * along with Pire.  If not, see <http://www.gnu.org/licenses>.
 */


#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <pire/pire.h>

class Timer {
public:
	Timer(const std::string& msg, size_t sz): m_msg(msg), m_sz(sz) { gettimeofday(&m_tv, 0); }
    
	~Timer()
	{
		struct timeval end;
		gettimeofday(&end, 0);
		long long usec = (end.tv_sec - m_tv.tv_sec) * 1000000 + (end.tv_usec - m_tv.tv_usec);
		float bw = m_sz *1.0 / usec;
		std::cerr << m_msg << ": " << usec << " us\t" << bw << " MB/sec" <<  std::endl;
	}
    
private:
	std::string m_msg;
	struct timeval m_tv;
	size_t m_sz;
};

typedef std::vector<Pire::Fsm> Fsms;

class ITester {
public:
	virtual ~ITester() {}
	virtual void Compile(const std::vector<Fsms>& fsms) = 0;
	virtual void Run(const char* begin, const char* end) = 0;
};

template<class Scanner>
struct Compile {
	static Scanner Do(const Fsms& fsms)
	{
		if (fsms.size() != 1)
			throw std::runtime_error("Only one regexp is allowed for this scanner");
		return Pire::Fsm(fsms[0]).Compile<Scanner>();
	}
};

template<class Relocation>
struct Compile< Pire::Impl::Scanner<Relocation> > {
	static Pire::Scanner Do(const Fsms& fsms)
	{
		Pire::Scanner sc;
		for (Fsms::const_iterator i = fsms.begin(), ie = fsms.end(); i != ie; ++i) {
			Pire::Scanner tsc = Pire::Fsm(*i).Compile<Pire::Scanner>();
			if (i == fsms.begin())
				tsc.Swap(sc);
			else {
				sc = Pire::Scanner::Glue(sc, tsc);
				if (sc.Empty()) {
					std::ostringstream msg;
					msg << "Scanner gluing failed at regexp #" << i - fsms.begin() << " - pattern too complicated";
					throw std::runtime_error(msg.str());
				}
			}
		}
		return sc;
	}
};

template<class Scanner>
struct PrintResult {
	static void Do(const Scanner& sc, typename Scanner::State st)
	{
		if (sc.Final(st))
			std::cerr << "Match" << std::endl;
		else
			std::cerr << "No match" << std::endl;
	}
};

template<class Relocation>
struct PrintResult< Pire::Impl::Scanner<Relocation> > {
	typedef Pire::Impl::Scanner<Relocation> Scanner;

	static void Do(const Scanner& sc, typename Scanner::State st)
	{
		std::pair<const size_t*, const size_t*> accepted = sc.AcceptedRegexps(st);
		std::cerr << "Accepted regexps:";
		for (; accepted.first != accepted.second; ++accepted.first)
			std::cerr << " " << *accepted.first;
		std::cerr << std::endl;
	}
};


template<class Scanner>
class Tester: public ITester {
public:
	void Compile(const std::vector<Fsms>& fsms)
	{
		if (fsms.size() != 1)
			throw std::runtime_error("Only one set of regexps is allowed for this scanner");
		sc = ::Compile<Scanner>::Do(fsms[0]);
	}
	
	void Run(const char* begin, const char* end)
	{
		PrintResult<Scanner>::Do(sc, Pire::Runner(sc).Begin().Run(begin, end).End().State());
	}
private:
	Scanner sc;
};

template<class Scanner1, class Scanner2>
class PairTester: public ITester {
	void Compile(const std::vector<Fsms>& fsms)
	{
		if (fsms.size() != 2)
			throw std::runtime_error("Only two sets of regexps are allowed for this scanner");
		sc1 = ::Compile<Scanner1>::Do(fsms[0]);
		sc2 = ::Compile<Scanner2>::Do(fsms[1]);
	}
	
	void Run(const char* begin, const char* end)
	{
		typedef Pire::Impl::ScannerPair<Scanner1, Scanner2> Pair;
		Pire::RunHelper<Pair> rh(Pair(sc1, sc2));
		rh.Begin().Run(begin, end).End();
		std::cerr << "[first] "; PrintResult<Scanner1>::Do(sc1, rh.State().first);
		std::cerr << "[second] "; PrintResult<Scanner2>::Do(sc2, rh.State().second);
	}
	
private:
	Scanner1 sc1;
	Scanner2 sc2;
};


class FileMmap {
public:
	explicit FileMmap(const char *name)
		: m_fd(0)
		, m_mmap(0)
		, m_len(0)
	{
		try {
			int fd = open(name, O_RDONLY);
			if (fd == -1)
				throw std::runtime_error(std::string("open failed for ") + name + ": " + strerror(errno));
			m_fd = fd;
			struct stat fileStat;
			int err = fstat(m_fd, &fileStat);
			if (err)
				throw std::runtime_error(std::string("fstat failed for") + name + ": " + strerror(errno));
			m_len = fileStat.st_size;
			const char* addr = (const char*)mmap(0, m_len, PROT_READ, MAP_PRIVATE, m_fd, 0);
			if (addr == MAP_FAILED)
				throw std::runtime_error(std::string("mmap failed for ") + name + ": " + strerror(errno));
			m_mmap = addr;
		} catch (...) {
			Close();
			throw;
		}
	}
	~FileMmap() { Close(); }
	size_t Size() const { return m_len; }
	const char* Begin() const { return m_mmap; }
	const char* End() const { return m_mmap + m_len; }

private:
	void Close()
	{
		if (m_mmap)
			munmap((void*)m_mmap, m_len);
		if (m_fd)
			close(m_fd);
		m_fd = 0;
		m_mmap = 0;
		m_len = 0;
	}

	int m_fd;
	const char* m_mmap;
	size_t m_len;
};

class MemTester: public ITester {
public:
	void Compile(const std::vector<Fsms>&) {}
	// Just estimates memory throughput
	void Run(const char* begin, const char* end)
	{
		size_t c = 0;
		const size_t *b = (const size_t*)begin;
		const size_t *e = (const size_t*)end;
		while (b < e) { c ^= *b++; }
		std::clog << c << std::endl;
	}
};


void Main(int argc, char** argv)
{
	std::runtime_error usage("Usage: bench -f file -t {multi|nonreloc|simple|slow|null} regexp [regexp2 [-e regexp3...]] [-t <type> regexp4 [regexp5...]]");
	std::vector<Fsms> fsms;
	std::vector<std::string> types;
	std::string file;
	for (--argc, ++argv; argc; --argc, ++argv) {
		if (!strcmp(*argv, "-t") && argc >= 2) {
			types.push_back(argv[1]);
			fsms.push_back(Fsms());
			--argc, ++argv;
		} else if (!strcmp(*argv, "-f") && argc >= 2) {
			file = argv[1];
			--argc, ++argv;
		} else if (!strcmp(*argv, "-e") && argc >= 2) {
			if (fsms.empty())
				throw usage;
			fsms.back().push_back(Pire::Lexer(std::string(argv[1])).Parse().Surround());
			--argc, ++argv;
		} else {
			if (fsms.empty())
				throw usage;
			fsms.back().push_back(Pire::Lexer(std::string(*argv)).Parse().Surround());
		}
	}
	if (types.empty() || file.empty() || fsms.back().empty())
		throw usage;

	std::auto_ptr<ITester> tester;
	
	// TODO: is there a way to get out of this copypasting?
	if (types.size() == 1 && types[0] == "multi")
		tester.reset(new Tester<Pire::Scanner>);
	else if (types.size() == 1 && types[0] == "nonreloc")
		tester.reset(new Tester<Pire::NonrelocScanner>);
	else if (types.size() == 1 && types[0] == "simple")
		tester.reset(new Tester<Pire::SimpleScanner>);
	else if (types.size() == 1 && types[0] == "slow")
		tester.reset(new Tester<Pire::SlowScanner>);
	else if (types.size() == 1 && types[0] == "null")
		tester.reset(new MemTester);
	else if (types.size() == 2 && types[0] == "multi" && types[1] == "multi")
		tester.reset(new PairTester<Pire::Scanner, Pire::Scanner>);
	else if (types.size() == 2 && types[0] == "multi" && types[1] == "simple")
		tester.reset(new PairTester<Pire::Scanner, Pire::SimpleScanner>);
	else if (types.size() == 2 && types[0] == "multi" && types[1] == "nonreloc")
		tester.reset(new PairTester<Pire::Scanner, Pire::NonrelocScanner>);
	else if (types.size() == 2 && types[0] == "simple" && types[1] == "simple")
		tester.reset(new PairTester<Pire::SimpleScanner, Pire::SimpleScanner>);
	else if (types.size() == 2 && types[0] == "simple" && types[1] == "multi")
		tester.reset(new PairTester<Pire::SimpleScanner, Pire::Scanner>);
	else if (types.size() == 2 && types[0] == "simple" && types[1] == "nonreloc")
		tester.reset(new PairTester<Pire::SimpleScanner, Pire::NonrelocScanner>);
	else if (types.size() == 2 && types[0] == "nonreloc" && types[1] == "multi")
		tester.reset(new PairTester<Pire::NonrelocScanner, Pire::Scanner>);
	else if (types.size() == 2 && types[0] == "nonreloc" && types[1] == "simple")
		tester.reset(new PairTester<Pire::NonrelocScanner, Pire::SimpleScanner>);
	else if (types.size() == 2 && types[0] == "nonreloc" && types[1] == "nonreloc")
		tester.reset(new PairTester<Pire::NonrelocScanner, Pire::NonrelocScanner>);

	else
		throw usage;

	tester->Compile(fsms);
	FileMmap fmap(file.c_str());

	// Run the benchmark multiple times
	std::ostringstream stream;
	for (std::vector<std::string>::iterator j = types.begin(), je = types.end(); j != je; ++j)
		stream << *j << " ";
	std::string typesName = stream.str();
	for (int i = 0; i < 3; ++i)
	{
		Timer timer(typesName, fmap.Size());
		tester->Run(fmap.Begin(), fmap.End());
	}
}

int main(int argc, char** argv)
{
	try {
		Main(argc, argv);
		return 0;
	}
	catch (std::exception& e) {
		std::cerr << "bench: " << e.what() << std::endl;
		return 1;
	}
}