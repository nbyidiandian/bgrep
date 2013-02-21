#include <fcntl.h>
#include <string>
#include <vector>
#include <cassert>

#include <iostream>
#include <fstream>
#include <sstream>

class Reader
{
public:
    Reader()
    { }
    virtual ~Reader()
    { }
    virtual int read(char *buffer, size_t buffer_size) = 0;
    virtual uint64_t tell() const = 0;
};

class StreamReader : public Reader
{
public:
    StreamReader(std::istream *stream);
    virtual ~StreamReader();
    virtual int read(char *buffer, size_t buffer_size);
    virtual uint64_t tell() const;
    
private:
    std::istream *_stream;
};

StreamReader::StreamReader(std::istream *stream)
        : Reader(),
          _stream(stream)
{ }

StreamReader::~StreamReader()
{ }

int StreamReader::read(char *buffer, size_t buffer_size)
{
    assert(_stream != NULL);
    if (_stream->eof())
        return -1;
    _stream->read(buffer, buffer_size);
    return _stream->gcount();
}

uint64_t StreamReader::tell() const
{
    return _stream->tellg();
}

class FileReader : public Reader
{
public:
    FileReader();
    virtual ~FileReader();
    bool open(const std::string &path);
    virtual int read(char *buffer, size_t buffer_size);
    virtual uint64_t tell() const;

private:
    int _fd;
    uint64_t _pos;
};

FileReader::FileReader() : _fd(-1), _pos(0)
{ }

FileReader::~FileReader()
{
    if (_fd != -1)
    {
        ::close(_fd);
        _fd = -1;
    }
}

bool FileReader::open(const std::string &path)
{
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    _fd = fd;
    return true;
}

int FileReader::read(char *buffer, size_t buffer_size)
{
    int nread = ::read(_fd, buffer, buffer_size);
    _pos += nread;
    return nread;
}

uint64_t FileReader::tell() const
{
    return _pos;
}

class Collector
{
public:
    Collector()
    { }
    virtual ~Collector()
    { }

    virtual void collect(const char *buffer, size_t buffer_size) = 0;
};

class StreamCollector : public Collector
{
public:
    StreamCollector(std::ostream *stream);
    virtual ~StreamCollector();
    virtual void collect(const char *buffer, size_t buffer_size);
    
private:
    std::ostream *_stream;
};

StreamCollector::StreamCollector(std::ostream *stream)
        : Collector(),
          _stream(stream)
{ }

StreamCollector::~StreamCollector()
{ }

void StreamCollector::collect(const char *buffer, size_t buffer_size)
{
    assert(_stream != NULL);
    _stream->write(buffer, buffer_size);
}

class Target
{
public:
    Target()
    { }
    ~Target()
    { }
    
    virtual bool match(const char *str, size_t len) const;
    void addTarget(const std::string &target);
    
private:
    std::vector<std::string> _targets;
};

void Target::addTarget(const std::string &target)
{
    _targets.push_back(target);
}

bool Target::match(const char *str, size_t len) const
{
    bool matched = false;
    for (size_t i = 0; i < _targets.size(); ++i)
    {
        const std::string &target = _targets[i];
        if (memmem(str, len, target.c_str(), target.size()) != NULL)
        {
            matched = true;
            break;
        }
    }
    return matched;
}

class RingBuffer
{
public:
    RingBuffer(int buf_num, int buf_size, const Target *target);
    ~RingBuffer();
    char *getBuffer(int buffer_idx);
    inline int getBufferSize() const;
    bool readFrom(Reader *reader, Collector *collector);
    void collectTo(int start, Collector *collector) const;
    
private:
    char **_buffer;
    int _buffer_num;
    int _buffer_size;
    int _next_buffer_idx;
    int _matched_buffer_idx;
    const Target *_target;
    char *_buffer_print_flag; // 0: not printed
};

RingBuffer::RingBuffer(int buffer_num, int buffer_size, const Target *target)
        : _buffer_num(buffer_num),
          _buffer_size(buffer_size),
          _next_buffer_idx(0),
          _matched_buffer_idx(-1),
          _target(target)
{
    assert(_buffer_num > 0);
    assert(_buffer_size > 0);
    _buffer = new char *[_buffer_num];
    _buffer_print_flag = new char[_buffer_num];
    for (int i = 0; i < _buffer_num; ++i)
    {
        _buffer[i] = new char[_buffer_size];
        _buffer_print_flag[i] = 1;
    }
}

RingBuffer::~RingBuffer()
{
    for (int i = 0; i < _buffer_num; ++i)
        delete _buffer[i];
    delete _buffer;
    _buffer = NULL;
}

bool RingBuffer::readFrom(Reader *reader, Collector *collector)
{
    int buffer_idx = _next_buffer_idx++;
    _next_buffer_idx %= _buffer_num;
    char *buffer = _buffer[buffer_idx];
    
    int nread = reader->read(buffer, _buffer_size);
    _buffer_print_flag[buffer_idx] = 0;

    uint64_t total_read = reader->tell();
    if (total_read % (1024 * 1024 * 1024) == 0)
    {
        fprintf(stderr, "%llu scanned\n", (total_read / (1024 * 1024 * 1024)));
    }

    if (nread <= 0)
    {
        if (_matched_buffer_idx >= 0)
        {
            int start = _matched_buffer_idx + _buffer_num / 2;
            start %= _buffer_num;
            collectTo(start, collector);
            _matched_buffer_idx = -1;
        }
        return false;
    }

    if (_matched_buffer_idx < 0 && _target->match(buffer, nread))
    {
        fprintf(stderr, "%llu matched\n", reader->tell());
        _matched_buffer_idx = buffer_idx;
    }

    if (_matched_buffer_idx >= 0)
    {
        int start = _matched_buffer_idx + _buffer_num / 2;
        start %= _buffer_num;
        if (_next_buffer_idx == start)
        {
            collectTo(start, collector);
            _matched_buffer_idx = -1;
        }
    }

    return true;
}

void RingBuffer::collectTo(int start, Collector *collector) const
{
    for (int i = start; i < _buffer_num; ++i)
    {
        if (_buffer_print_flag[i] == 0)
        {
            collector->collect(_buffer[i], _buffer_size);
            _buffer_print_flag[i] = 1;
        }
    }
    for (int i = 0; i < start; ++i)
    {
        if (_buffer_print_flag[i] == 0)
        {
            collector->collect(_buffer[i], _buffer_size);
            _buffer_print_flag[i] = 1;
        }
    }
}

int test()
{
#define TEST_ASSERT(x) \
    if(!(x)) { fprintf(stderr, "assert failed %s:%d\n", __FILE__, __LINE__); return -1; } \
    else { fprintf(stderr, "."); }
    Target target;
    target.addTarget("ab");
    {
        RingBuffer buffer(4, 4, &target);
        std::istringstream is("000011112222aaab3333bbbb4444cccc5555aaab666677778888");
        std::ostringstream os;
        StreamReader reader(&is);
        StreamCollector collector(&os);
        while (buffer.readFrom(&reader, &collector))
        { }
        TEST_ASSERT(os.str() == "11112222aaab3333cccc5555aaab6666");
    }
    {
        RingBuffer buffer(4, 4, &target);
        std::istringstream is("000011112222aaab3333bbbbaaab666677778888");
        std::ostringstream os;
        StreamReader reader(&is);
        StreamCollector collector(&os);
        while (buffer.readFrom(&reader, &collector))
        { }
        TEST_ASSERT(os.str() == "11112222aaab3333bbbbaaab6666");
    }
    {
        RingBuffer buffer(4, 4, &target);
        std::istringstream is("000011112222aaab3333bbbb4444cccc");
        std::ostringstream os;
        StreamReader reader(&is);
        StreamCollector collector(&os);
        while (buffer.readFrom(&reader, &collector))
        { }
        TEST_ASSERT(os.str() == "11112222aaab3333");
    }
    {
        RingBuffer buffer(4, 4, &target);
        std::istringstream is("aaab0000111122223333bbbb4444cccc");
        std::ostringstream os;
        StreamReader reader(&is);
        StreamCollector collector(&os);
        while (buffer.readFrom(&reader, &collector))
        { }
        TEST_ASSERT(os.str() == "aaab0000");
    }
    {
        RingBuffer buffer(4, 4, &target);
        std::istringstream is("0000111122223333bbbb4444ccccaaab");
        std::ostringstream os;
        StreamReader reader(&is);
        StreamCollector collector(&os);
        while (buffer.readFrom(&reader, &collector))
        { }
        fprintf(stdout, "%s\n", os.str().c_str());
        TEST_ASSERT(os.str() == "aaab0000");
    }
    return 0;
}

int run(const char *dev, int argc, const char *argv[])
{
    std::ifstream is(dev);
    if (!is.is_open())
    {
        perror("open file failed");
        return -1;
    }
    Target target;
    for (int i = 0; i < argc; ++i)
        target.addTarget(argv[i]);
    
    RingBuffer buffer(16, 4096, &target);

    StreamReader reader(&is);
    StreamCollector collector(&std::cout);
    while (buffer.readFrom(&reader, &collector))
    { }
    return 0;
}

int main(int argc, const char *argv[])
{
    // return test();
    if (argc < 3 ) {
        printf("Usage: %s /dev/sda mark\n", argv[0]);
        return 1;
    }
    return run(argv[1], argc - 2, argv + 2);
}



/*
int main(int argc, const char *argv[])
{

    const static char END[] = "========FOUND=======";
    const char *need = argv[2];
	size_t need_len = strlen(need);
    static const int BUF_NUM = 10;
    static const int BUF_SIZE = 32;
    char buf[BUF_NUM][BUF_SIZE];
    int buf_idx = 0;
    int rd_count = 0;
	uint64_t total_read_count = 0;
    int found = 0;
    int fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		perror("open file failed");
		return -1;
	}

	for (; (rd_count = read(fd, buf[buf_idx], BUF_SIZE)) > 0; ++buf_idx) {
        buf_idx %= BUF_NUM;
		total_read_count += rd_count;
		uint64_t mod = total_read_count % (1024*1024*1024);
		if (mod == 0) {
			fprintf(stderr, "%lluG byte scanned\n", total_read_count / (1024*1024*1024));
		}

        if (found >= BUF_NUM / 2) {
            for (int j = buf_idx; j < BUF_NUM + buf_idx; ++j) {
                fwrite(buf[j % BUF_NUM], 1, BUF_SIZE, stdout);
            }
            fwrite(END, 1, sizeof(END), stdout);
            found = 0;
            continue;
        }

        if (found > 0) {
            ++found;
            continue;
        }

        if (found == 0
                && memmem(buf[buf_idx], rd_count, need, need_len) != NULL) {
            for (int j = buf_idx+1; j < BUF_NUM + buf_idx; ++j) {
				memset(buf[j % BUF_NUM], '0', BUF_SIZE);
            }
            found = 1;
        }
    }

    if (found > 0) {
        for (int j = 0; j < BUF_NUM; ++j) {
            fwrite(buf[j], 1, BUF_SIZE, stdout);
        }
    }

    return 0;
}
*/
