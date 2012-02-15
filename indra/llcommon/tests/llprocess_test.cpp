/**
 * @file   llprocess_test.cpp
 * @author Nat Goodspeed
 * @date   2011-12-19
 * @brief  Test for llprocess.
 * 
 * $LicenseInfo:firstyear=2011&license=viewerlgpl$
 * Copyright (c) 2011, Linden Research, Inc.
 * $/LicenseInfo$
 */

// Precompiled header
#include "linden_common.h"
// associated header
#include "llprocess.h"
// STL headers
#include <vector>
#include <list>
// std headers
#include <fstream>
// external library headers
#include "llapr.h"
#include "apr_thread_proc.h"
#include <boost/foreach.hpp>
#include <boost/function.hpp>
#include <boost/algorithm/string/find_iterator.hpp>
#include <boost/algorithm/string/finder.hpp>
//#include <boost/lambda/lambda.hpp>
//#include <boost/lambda/bind.hpp>
// other Linden headers
#include "../test/lltut.h"
#include "../test/manageapr.h"
#include "../test/namedtempfile.h"
#include "stringize.h"
#include "llsdutil.h"
#include "llevents.h"
#include "llerrorcontrol.h"

#if defined(LL_WINDOWS)
#define sleep(secs) _sleep((secs) * 1000)
#define EOL "\r\n"
#else
#define EOL "\n"
#include <sys/wait.h>
#endif

//namespace lambda = boost::lambda;

// static instance of this manages APR init/cleanup
static ManageAPR manager;

/*****************************************************************************
*   Helpers
*****************************************************************************/

#define ensure_equals_(left, right) \
        ensure_equals(STRINGIZE(#left << " != " << #right), (left), (right))

#define aprchk(expr) aprchk_(#expr, (expr))
static void aprchk_(const char* call, apr_status_t rv, apr_status_t expected=APR_SUCCESS)
{
    tut::ensure_equals(STRINGIZE(call << " => " << rv << ": " << manager.strerror(rv)),
                       rv, expected);
}

/**
 * Read specified file using std::getline(). It is assumed to be an error if
 * the file is empty: don't use this function if that's an acceptable case.
 * Last line will not end with '\n'; this is to facilitate the usual case of
 * string compares with a single line of output.
 * @param pathname The file to read.
 * @param desc Optional description of the file for error message;
 * defaults to "in <pathname>"
 */
static std::string readfile(const std::string& pathname, const std::string& desc="")
{
    std::string use_desc(desc);
    if (use_desc.empty())
    {
        use_desc = STRINGIZE("in " << pathname);
    }
    std::ifstream inf(pathname.c_str());
    std::string output;
    tut::ensure(STRINGIZE("No output " << use_desc), std::getline(inf, output));
    std::string more;
    while (std::getline(inf, more))
    {
        output += '\n' + more;
    }
    return output;
}

/// Looping on LLProcess::isRunning() must now be accompanied by pumping
/// "mainloop" -- otherwise the status won't update and you get an infinite
/// loop.
void yield(int seconds=1)
{
    // This function simulates waiting for another viewer frame
    sleep(seconds);
    LLEventPumps::instance().obtain("mainloop").post(LLSD());
}

void waitfor(LLProcess& proc)
{
    while (proc.isRunning())
    {
        yield();
    }
}

void waitfor(LLProcess::handle h, const std::string& desc)
{
    while (LLProcess::isRunning(h, desc))
    {
        yield();
    }
}

/**
 * Construct an LLProcess to run a Python script.
 */
struct PythonProcessLauncher
{
    /**
     * @param desc Arbitrary description for error messages
     * @param script Python script, any form acceptable to NamedTempFile,
     * typically either a std::string or an expression of the form
     * (lambda::_1 << "script content with " << variable_data)
     */
    template <typename CONTENT>
    PythonProcessLauncher(const std::string& desc, const CONTENT& script):
        mDesc(desc),
        mScript("py", script)
    {
        const char* PYTHON(getenv("PYTHON"));
        tut::ensure("Set $PYTHON to the Python interpreter", PYTHON);

        mParams.executable = PYTHON;
        mParams.args.add(mScript.getName());
    }

    /// Run Python script and wait for it to complete.
    void run()
    {
        mPy = LLProcess::create(mParams);
        tut::ensure(STRINGIZE("Couldn't launch " << mDesc << " script"), mPy);
        // One of the irritating things about LLProcess is that
        // there's no API to wait for the child to terminate -- but given
        // its use in our graphics-intensive interactive viewer, it's
        // understandable.
        waitfor(*mPy);
    }

    /**
     * Run a Python script using LLProcess, expecting that it will
     * write to the file passed as its sys.argv[1]. Retrieve that output.
     *
     * Until January 2012, LLProcess provided distressingly few
     * mechanisms for a child process to communicate back to its caller --
     * not even its return code. We've introduced a convention by which we
     * create an empty temp file, pass the name of that file to our child
     * as sys.argv[1] and expect the script to write its output to that
     * file. This function implements the C++ (parent process) side of
     * that convention.
     */
    std::string run_read()
    {
        NamedTempFile out("out", ""); // placeholder
        // pass name of this temporary file to the script
        mParams.args.add(out.getName());
        run();
        // assuming the script wrote to that file, read it
        return readfile(out.getName(), STRINGIZE("from " << mDesc << " script"));
    }

    LLProcess::Params mParams;
    LLProcessPtr mPy;
    std::string mDesc;
    NamedTempFile mScript;
};

/// convenience function for PythonProcessLauncher::run()
template <typename CONTENT>
static void python(const std::string& desc, const CONTENT& script)
{
    PythonProcessLauncher py(desc, script);
    py.run();
}

/// convenience function for PythonProcessLauncher::run_read()
template <typename CONTENT>
static std::string python_out(const std::string& desc, const CONTENT& script)
{
    PythonProcessLauncher py(desc, script);
    return py.run_read();
}

/// Create a temporary directory and clean it up later.
class NamedTempDir: public boost::noncopyable
{
public:
    // Use python() function to create a temp directory: I've found
    // nothing in either Boost.Filesystem or APR quite like Python's
    // tempfile.mkdtemp().
    // Special extra bonus: on Mac, mkdtemp() reports a pathname
    // starting with /var/folders/something, whereas that's really a
    // symlink to /private/var/folders/something. Have to use
    // realpath() to compare properly.
    NamedTempDir():
        mPath(python_out("mkdtemp()",
                         "from __future__ import with_statement\n"
                         "import os.path, sys, tempfile\n"
                         "with open(sys.argv[1], 'w') as f:\n"
                         "    f.write(os.path.normcase(os.path.normpath(os.path.realpath(tempfile.mkdtemp()))))\n"))
    {}

    ~NamedTempDir()
    {
        aprchk(apr_dir_remove(mPath.c_str(), gAPRPoolp));
    }

    std::string getName() const { return mPath; }

private:
    std::string mPath;
};

// statically reference the function in test.cpp... it's short, we could
// replicate, but better to reuse
extern void wouldHaveCrashed(const std::string& message);

/**
 * Capture log messages. This is adapted (simplified) from the one in
 * llerror_test.cpp. Sigh, should've broken that out into a separate header
 * file, but time for this project is short...
 */
class TestRecorder : public LLError::Recorder
{
public:
    TestRecorder():
        // Mostly what we're trying to accomplish by saving and resetting
        // LLError::Settings is to bypass the default RecordToStderr and
        // RecordToWinDebug Recorders. As these are visible only inside
        // llerror.cpp, we can't just call LLError::removeRecorder() with
        // each. For certain tests we need to produce, capture and examine
        // DEBUG log messages -- but we don't want to spam the user's console
        // with that output. If it turns out that saveAndResetSettings() has
        // some bad effect, give up and just let the DEBUG level log messages
        // display.
        mOldSettings(LLError::saveAndResetSettings())
    {
        LLError::setFatalFunction(wouldHaveCrashed);
        LLError::setDefaultLevel(LLError::LEVEL_DEBUG);
        LLError::addRecorder(this);
    }

    ~TestRecorder()
    {
        LLError::removeRecorder(this);
        LLError::restoreSettings(mOldSettings);
    }

    void recordMessage(LLError::ELevel level,
                       const std::string& message)
    {
        mMessages.push_back(message);
    }

    /// Don't assume the message we want is necessarily the LAST log message
    /// emitted by the underlying code; search backwards through all messages
    /// for the sought string.
    std::string messageWith(const std::string& search)
    {
        for (std::list<std::string>::const_reverse_iterator rmi(mMessages.rbegin()),
                 rmend(mMessages.rend());
             rmi != rmend; ++rmi)
        {
            if (rmi->find(search) != std::string::npos)
                return *rmi;
        }
        // failed to find any such message
        return std::string();
    }

    typedef std::list<std::string> MessageList;
    MessageList mMessages;
    LLError::Settings* mOldSettings;
};

/*****************************************************************************
*   TUT
*****************************************************************************/
namespace tut
{
    struct llprocess_data
    {
        LLAPRPool pool;
    };
    typedef test_group<llprocess_data> llprocess_group;
    typedef llprocess_group::object object;
    llprocess_group llprocessgrp("llprocess");

    struct Item
    {
        Item(): tries(0) {}
        unsigned    tries;
        std::string which;
        std::string what;
    };

/*==========================================================================*|
#define tabent(symbol) { symbol, #symbol }
    static struct ReasonCode
    {
        int code;
        const char* name;
    } reasons[] =
    {
        tabent(APR_OC_REASON_DEATH),
        tabent(APR_OC_REASON_UNWRITABLE),
        tabent(APR_OC_REASON_RESTART),
        tabent(APR_OC_REASON_UNREGISTER),
        tabent(APR_OC_REASON_LOST),
        tabent(APR_OC_REASON_RUNNING)
    };
#undef tabent
|*==========================================================================*/

    struct WaitInfo
    {
        WaitInfo(apr_proc_t* child_):
            child(child_),
            rv(-1),                 // we haven't yet called apr_proc_wait()
            rc(0),
            why(apr_exit_why_e(0))
        {}
        apr_proc_t* child;          // which subprocess
        apr_status_t rv;            // return from apr_proc_wait()
        int rc;                     // child's exit code
        apr_exit_why_e why;         // APR_PROC_EXIT, APR_PROC_SIGNAL, APR_PROC_SIGNAL_CORE
    };

    void child_status_callback(int reason, void* data, int status)
    {
/*==========================================================================*|
        std::string reason_str;
        BOOST_FOREACH(const ReasonCode& rcp, reasons)
        {
            if (reason == rcp.code)
            {
                reason_str = rcp.name;
                break;
            }
        }
        if (reason_str.empty())
        {
            reason_str = STRINGIZE("unknown reason " << reason);
        }
        std::cout << "child_status_callback(" << reason_str << ")\n";
|*==========================================================================*/

        if (reason == APR_OC_REASON_DEATH || reason == APR_OC_REASON_LOST)
        {
            // Somewhat oddly, APR requires that you explicitly unregister
            // even when it already knows the child has terminated.
            apr_proc_other_child_unregister(data);

            WaitInfo* wi(static_cast<WaitInfo*>(data));
            // It's just wrong to call apr_proc_wait() here. The only way APR
            // knows to call us with APR_OC_REASON_DEATH is that it's already
            // reaped this child process, so calling wait() will only produce
            // "huh?" from the OS. We must rely on the status param passed in,
            // which unfortunately comes straight from the OS wait() call.
//          wi->rv = apr_proc_wait(wi->child, &wi->rc, &wi->why, APR_NOWAIT);
            wi->rv = APR_CHILD_DONE; // fake apr_proc_wait() results
#if defined(LL_WINDOWS)
            wi->why = APR_PROC_EXIT;
            wi->rc  = status;         // no encoding on Windows (no signals)
#else  // Posix
            if (WIFEXITED(status))
            {
                wi->why = APR_PROC_EXIT;
                wi->rc  = WEXITSTATUS(status);
            }
            else if (WIFSIGNALED(status))
            {
                wi->why = APR_PROC_SIGNAL;
                wi->rc  = WTERMSIG(status);
            }
            else                    // uh, shouldn't happen?
            {
                wi->why = APR_PROC_EXIT;
                wi->rc  = status;   // someone else will have to decode
            }
#endif // Posix
        }
    }

    template<> template<>
    void object::test<1>()
    {
        set_test_name("raw APR nonblocking I/O");

        // Create a script file in a temporary place.
        NamedTempFile script("py",
            "import sys" EOL
            "import time" EOL
            EOL
            "time.sleep(2)" EOL
            "print >>sys.stdout, 'stdout after wait'" EOL
            "sys.stdout.flush()" EOL
            "time.sleep(2)" EOL
            "print >>sys.stderr, 'stderr after wait'" EOL
            "sys.stderr.flush()" EOL
            );

        // Arrange to track the history of our interaction with child: what we
        // fetched, which pipe it came from, how many tries it took before we
        // got it.
        std::vector<Item> history;
        history.push_back(Item());

        // Run the child process.
        apr_procattr_t *procattr = NULL;
        aprchk(apr_procattr_create(&procattr, pool.getAPRPool()));
        aprchk(apr_procattr_io_set(procattr, APR_CHILD_BLOCK, APR_CHILD_BLOCK, APR_CHILD_BLOCK));
        aprchk(apr_procattr_cmdtype_set(procattr, APR_PROGRAM_PATH));

        std::vector<const char*> argv;
        apr_proc_t child;
        argv.push_back("python");
        // Have to have a named copy of this std::string so its c_str() value
        // will persist.
        std::string scriptname(script.getName());
        argv.push_back(scriptname.c_str());
        argv.push_back(NULL);

        aprchk(apr_proc_create(&child, argv[0],
                               &argv[0],
                               NULL, // if we wanted to pass explicit environment
                               procattr,
                               pool.getAPRPool()));

        // We do not want this child process to outlive our APR pool. On
        // destruction of the pool, forcibly kill the process. Tell APR to try
        // SIGTERM and wait 3 seconds. If that didn't work, use SIGKILL.
        apr_pool_note_subprocess(pool.getAPRPool(), &child, APR_KILL_AFTER_TIMEOUT);

        // arrange to call child_status_callback()
        WaitInfo wi(&child);
        apr_proc_other_child_register(&child, child_status_callback, &wi, child.in, pool.getAPRPool());

        // TODO:
        // Stuff child.in until it (would) block to verify EWOULDBLOCK/EAGAIN.
        // Have child script clear it later, then write one more line to prove
        // that it gets through.

        // Monitor two different output pipes. Because one will be closed
        // before the other, keep them in a list so we can drop whichever of
        // them is closed first.
        typedef std::pair<std::string, apr_file_t*> DescFile;
        typedef std::list<DescFile> DescFileList;
        DescFileList outfiles;
        outfiles.push_back(DescFile("out", child.out));
        outfiles.push_back(DescFile("err", child.err));

        while (! outfiles.empty())
        {
            // This peculiar for loop is designed to let us erase(dfli). With
            // a list, that invalidates only dfli itself -- but even so, we
            // lose the ability to increment it for the next item. So at the
            // top of every loop, while dfli is still valid, increment
            // dflnext. Then before the next iteration, set dfli to dflnext.
            for (DescFileList::iterator
                     dfli(outfiles.begin()), dflnext(outfiles.begin()), dflend(outfiles.end());
                 dfli != dflend; dfli = dflnext)
            {
                // Only valid to increment dflnext once we're sure it's not
                // already at dflend.
                ++dflnext;

                char buf[4096];

                apr_status_t rv = apr_file_gets(buf, sizeof(buf), dfli->second);
                if (APR_STATUS_IS_EOF(rv))
                {
//                  std::cout << "(EOF on " << dfli->first << ")\n";
//                  history.back().which = dfli->first;
//                  history.back().what  = "*eof*";
//                  history.push_back(Item());
                    outfiles.erase(dfli);
                    continue;
                }
                if (rv == EWOULDBLOCK || rv == EAGAIN)
                {
//                  std::cout << "(waiting; apr_file_gets(" << dfli->first << ") => " << rv << ": " << manager.strerror(rv) << ")\n";
                    ++history.back().tries;
                    continue;
                }
                aprchk_("apr_file_gets(buf, sizeof(buf), dfli->second)", rv);
                // Is it even possible to get APR_SUCCESS but read 0 bytes?
                // Hope not, but defend against that anyway.
                if (buf[0])
                {
//                  std::cout << dfli->first << ": " << buf;
                    history.back().which = dfli->first;
                    history.back().what.append(buf);
                    if (buf[strlen(buf) - 1] == '\n')
                        history.push_back(Item());
                    else
                    {
                        // Just for pretty output... if we only read a partial
                        // line, terminate it.
//                      std::cout << "...\n";
                    }
                }
            }
            // Do this once per tick, as we expect the viewer will
            apr_proc_other_child_refresh_all(APR_OC_REASON_RUNNING);
            sleep(1);
        }
        apr_file_close(child.in);
        apr_file_close(child.out);
        apr_file_close(child.err);

        // Okay, we've broken the loop because our pipes are all closed. If we
        // haven't yet called wait, give the callback one more chance. This
        // models the fact that unlike this small test program, the viewer
        // will still be running.
        if (wi.rv == -1)
        {
            std::cout << "last gasp apr_proc_other_child_refresh_all()\n";
            apr_proc_other_child_refresh_all(APR_OC_REASON_RUNNING);
        }

        if (wi.rv == -1)
        {
            std::cout << "child_status_callback(APR_OC_REASON_DEATH) wasn't called" << std::endl;
            wi.rv = apr_proc_wait(wi.child, &wi.rc, &wi.why, APR_NOWAIT);
        }
//      std::cout << "child done: rv = " << rv << " (" << manager.strerror(rv) << "), why = " << why << ", rc = " << rc << '\n';
        aprchk_("apr_proc_wait(wi->child, &wi->rc, &wi->why, APR_NOWAIT)", wi.rv, APR_CHILD_DONE);
        ensure_equals_(wi.why, APR_PROC_EXIT);
        ensure_equals_(wi.rc, 0);

        // Beyond merely executing all the above successfully, verify that we
        // obtained expected output -- and that we duly got control while
        // waiting, proving the non-blocking nature of these pipes.
        try
        {
            unsigned i = 0;
            ensure("blocking I/O on child pipe (0)", history[i].tries);
            ensure_equals_(history[i].which, "out");
            ensure_equals_(history[i].what,  "stdout after wait" EOL);
//          ++i;
//          ensure_equals_(history[i].which, "out");
//          ensure_equals_(history[i].what,  "*eof*");
            ++i;
            ensure("blocking I/O on child pipe (1)", history[i].tries);
            ensure_equals_(history[i].which, "err");
            ensure_equals_(history[i].what,  "stderr after wait" EOL);
//          ++i;
//          ensure_equals_(history[i].which, "err");
//          ensure_equals_(history[i].what,  "*eof*");
        }
        catch (const failure&)
        {
            std::cout << "History:\n";
            BOOST_FOREACH(const Item& item, history)
            {
                std::string what(item.what);
                if ((! what.empty()) && what[what.length() - 1] == '\n')
                {
                    what.erase(what.length() - 1);
                    if ((! what.empty()) && what[what.length() - 1] == '\r')
                    {
                        what.erase(what.length() - 1);
                        what.append("\\r");
                    }
                    what.append("\\n");
                }
                std::cout << "  " << item.which << ": '" << what << "' ("
                          << item.tries << " tries)\n";
            }
            std::cout << std::flush;
            // re-raise same error; just want to enrich the output
            throw;
        }
    }

    template<> template<>
    void object::test<2>()
    {
        set_test_name("setWorkingDirectory()");
        // We want to test setWorkingDirectory(). But what directory is
        // guaranteed to exist on every machine, under every OS? Have to
        // create one. Naturally, ensure we clean it up when done.
        NamedTempDir tempdir;
        PythonProcessLauncher py("getcwd()",
                                 "from __future__ import with_statement\n"
                                 "import os, sys\n"
                                 "with open(sys.argv[1], 'w') as f:\n"
                                 "    f.write(os.path.normcase(os.path.normpath(os.getcwd())))\n");
        // Before running, call setWorkingDirectory()
        py.mParams.cwd = tempdir.getName();
        ensure_equals("os.getcwd()", py.run_read(), tempdir.getName());
    }

    template<> template<>
    void object::test<3>()
    {
        set_test_name("arguments");
        PythonProcessLauncher py("args",
                                 "from __future__ import with_statement\n"
                                 "import sys\n"
                                 // note nonstandard output-file arg!
                                 "with open(sys.argv[3], 'w') as f:\n"
                                 "    for arg in sys.argv[1:]:\n"
                                 "        print >>f, arg\n");
        // We expect that PythonProcessLauncher has already appended
        // its own NamedTempFile to mParams.args (sys.argv[0]).
        py.mParams.args.add("first arg");          // sys.argv[1]
        py.mParams.args.add("second arg");         // sys.argv[2]
        // run_read() appends() one more argument, hence [3]
        std::string output(py.run_read());
        boost::split_iterator<std::string::const_iterator>
            li(output, boost::first_finder("\n")), lend;
        ensure("didn't get first arg", li != lend);
        std::string arg(li->begin(), li->end());
        ensure_equals(arg, "first arg");
        ++li;
        ensure("didn't get second arg", li != lend);
        arg.assign(li->begin(), li->end());
        ensure_equals(arg, "second arg");
        ++li;
        ensure("didn't get output filename?!", li != lend);
        arg.assign(li->begin(), li->end());
        ensure("output filename empty?!", ! arg.empty());
        ++li;
        ensure("too many args", li == lend);
    }

    template<> template<>
    void object::test<4>()
    {
        set_test_name("exit(0)");
        PythonProcessLauncher py("exit(0)",
                                 "import sys\n"
                                 "sys.exit(0)\n");
        py.run();
        ensure_equals("Status.mState", py.mPy->getStatus().mState, LLProcess::EXITED);
        ensure_equals("Status.mData",  py.mPy->getStatus().mData,  0);
    }

    template<> template<>
    void object::test<5>()
    {
        set_test_name("exit(2)");
        PythonProcessLauncher py("exit(2)",
                                 "import sys\n"
                                 "sys.exit(2)\n");
        py.run();
        ensure_equals("Status.mState", py.mPy->getStatus().mState, LLProcess::EXITED);
        ensure_equals("Status.mData",  py.mPy->getStatus().mData,  2);
    }

    template<> template<>
    void object::test<6>()
    {
        set_test_name("syntax_error:");
        PythonProcessLauncher py("syntax_error:",
                                 "syntax_error:\n");
        py.mParams.files.add(LLProcess::FileParam()); // inherit stdin
        py.mParams.files.add(LLProcess::FileParam()); // inherit stdout
        py.mParams.files.add(LLProcess::FileParam("pipe")); // pipe for stderr
        py.run();
        ensure_equals("Status.mState", py.mPy->getStatus().mState, LLProcess::EXITED);
        ensure_equals("Status.mData",  py.mPy->getStatus().mData,  1);
        std::istream& rpipe(py.mPy->getReadPipe(LLProcess::STDERR).get_istream());
        std::vector<char> buffer(4096);
        rpipe.read(&buffer[0], buffer.size());
        std::streamsize got(rpipe.gcount());
        ensure("Nothing read from stderr pipe", got);
        std::string data(&buffer[0], got);
        ensure("Didn't find 'SyntaxError:'", data.find("\nSyntaxError:") != std::string::npos);
    }

    template<> template<>
    void object::test<7>()
    {
        set_test_name("explicit kill()");
        PythonProcessLauncher py("kill()",
                                 "from __future__ import with_statement\n"
                                 "import sys, time\n"
                                 "with open(sys.argv[1], 'w') as f:\n"
                                 "    f.write('ok')\n"
                                 "# now sleep; expect caller to kill\n"
                                 "time.sleep(120)\n"
                                 "# if caller hasn't managed to kill by now, bad\n"
                                 "with open(sys.argv[1], 'w') as f:\n"
                                 "    f.write('bad')\n");
        NamedTempFile out("out", "not started");
        py.mParams.args.add(out.getName());
        py.mPy = LLProcess::create(py.mParams);
        ensure("couldn't launch kill() script", py.mPy);
        // Wait for the script to wake up and do its first write
        int i = 0, timeout = 60;
        for ( ; i < timeout; ++i)
        {
            yield();
            if (readfile(out.getName(), "from kill() script") == "ok")
                break;
        }
        // If we broke this loop because of the counter, something's wrong
        ensure("script never started", i < timeout);
        // script has performed its first write and should now be sleeping.
        py.mPy->kill();
        // wait for the script to terminate... one way or another.
        waitfor(*py.mPy);
#if LL_WINDOWS
        ensure_equals("Status.mState", py.mPy->getStatus().mState, LLProcess::EXITED);
        ensure_equals("Status.mData",  py.mPy->getStatus().mData,  -1);
#else
        ensure_equals("Status.mState", py.mPy->getStatus().mState, LLProcess::KILLED);
        ensure_equals("Status.mData",  py.mPy->getStatus().mData,  SIGTERM);
#endif
        // If kill() failed, the script would have woken up on its own and
        // overwritten the file with 'bad'. But if kill() succeeded, it should
        // not have had that chance.
        ensure_equals("kill() script output", readfile(out.getName()), "ok");
    }

    template<> template<>
    void object::test<8>()
    {
        set_test_name("implicit kill()");
        NamedTempFile out("out", "not started");
        LLProcess::handle phandle(0);
        {
            PythonProcessLauncher py("kill()",
                                     "from __future__ import with_statement\n"
                                     "import sys, time\n"
                                     "with open(sys.argv[1], 'w') as f:\n"
                                     "    f.write('ok')\n"
                                     "# now sleep; expect caller to kill\n"
                                     "time.sleep(120)\n"
                                     "# if caller hasn't managed to kill by now, bad\n"
                                     "with open(sys.argv[1], 'w') as f:\n"
                                     "    f.write('bad')\n");
            py.mParams.args.add(out.getName());
            py.mPy = LLProcess::create(py.mParams);
            ensure("couldn't launch kill() script", py.mPy);
            // Capture handle for later
            phandle = py.mPy->getProcessHandle();
            // Wait for the script to wake up and do its first write
            int i = 0, timeout = 60;
            for ( ; i < timeout; ++i)
            {
                yield();
                if (readfile(out.getName(), "from kill() script") == "ok")
                    break;
            }
            // If we broke this loop because of the counter, something's wrong
            ensure("script never started", i < timeout);
            // Script has performed its first write and should now be sleeping.
            // Destroy the LLProcess, which should kill the child.
        }
        // wait for the script to terminate... one way or another.
        waitfor(phandle, "kill() script");
        // If kill() failed, the script would have woken up on its own and
        // overwritten the file with 'bad'. But if kill() succeeded, it should
        // not have had that chance.
        ensure_equals("kill() script output", readfile(out.getName()), "ok");
    }

    template<> template<>
    void object::test<9>()
    {
        set_test_name("autokill=false");
        NamedTempFile from("from", "not started");
        NamedTempFile to("to", "");
        LLProcess::handle phandle(0);
        {
            PythonProcessLauncher py("autokill",
                                     "from __future__ import with_statement\n"
                                     "import sys, time\n"
                                     "with open(sys.argv[1], 'w') as f:\n"
                                     "    f.write('ok')\n"
                                     "# wait for 'go' from test program\n"
                                     "for i in xrange(60):\n"
                                     "    time.sleep(1)\n"
                                     "    with open(sys.argv[2]) as f:\n"
                                     "        go = f.read()\n"
                                     "    if go == 'go':\n"
                                     "        break\n"
                                     "else:\n"
                                     "    with open(sys.argv[1], 'w') as f:\n"
                                     "        f.write('never saw go')\n"
                                     "    sys.exit(1)\n"
                                     "# okay, saw 'go', write 'ack'\n"
                                     "with open(sys.argv[1], 'w') as f:\n"
                                     "    f.write('ack')\n");
            py.mParams.args.add(from.getName());
            py.mParams.args.add(to.getName());
            py.mParams.autokill = false;
            py.mPy = LLProcess::create(py.mParams);
            ensure("couldn't launch kill() script", py.mPy);
            // Capture handle for later
            phandle = py.mPy->getProcessHandle();
            // Wait for the script to wake up and do its first write
            int i = 0, timeout = 60;
            for ( ; i < timeout; ++i)
            {
                yield();
                if (readfile(from.getName(), "from autokill script") == "ok")
                    break;
            }
            // If we broke this loop because of the counter, something's wrong
            ensure("script never started", i < timeout);
            // Now destroy the LLProcess, which should NOT kill the child!
        }
        // If the destructor killed the child anyway, give it time to die
        yield(2);
        // How do we know it's not terminated? By making it respond to
        // a specific stimulus in a specific way.
        {
            std::ofstream outf(to.getName().c_str());
            outf << "go";
        } // flush and close.
        // now wait for the script to terminate... one way or another.
        waitfor(phandle, "autokill script");
        // If the LLProcess destructor implicitly called kill(), the
        // script could not have written 'ack' as we expect.
        ensure_equals("autokill script output", readfile(from.getName()), "ack");
    }

    template<> template<>
    void object::test<10>()
    {
        set_test_name("'bogus' test");
        TestRecorder recorder;
        PythonProcessLauncher py("'bogus' test",
                                 "print 'Hello world'\n");
        py.mParams.files.add(LLProcess::FileParam("bogus"));
        py.mPy = LLProcess::create(py.mParams);
        ensure("should have rejected 'bogus'", ! py.mPy);
        std::string message(recorder.messageWith("bogus"));
        ensure("did not log 'bogus' type", ! message.empty());
        ensure_contains("did not name 'stdin'", message, "stdin");
    }

    template<> template<>
    void object::test<11>()
    {
        set_test_name("'file' test");
        // Replace this test with one or more real 'file' tests when we
        // implement 'file' support
        PythonProcessLauncher py("'file' test",
                                 "print 'Hello world'\n");
        py.mParams.files.add(LLProcess::FileParam());
        py.mParams.files.add(LLProcess::FileParam("file"));
        py.mPy = LLProcess::create(py.mParams);
        ensure("should have rejected 'file'", ! py.mPy);
    }

    template<> template<>
    void object::test<12>()
    {
        set_test_name("'tpipe' test");
        // Replace this test with one or more real 'tpipe' tests when we
        // implement 'tpipe' support
        TestRecorder recorder;
        PythonProcessLauncher py("'tpipe' test",
                                 "print 'Hello world'\n");
        py.mParams.files.add(LLProcess::FileParam());
        py.mParams.files.add(LLProcess::FileParam("tpipe"));
        py.mPy = LLProcess::create(py.mParams);
        ensure("should have rejected 'tpipe'", ! py.mPy);
        std::string message(recorder.messageWith("tpipe"));
        ensure("did not log 'tpipe' type", ! message.empty());
        ensure_contains("did not name 'stdout'", message, "stdout");
    }

    template<> template<>
    void object::test<13>()
    {
        set_test_name("'npipe' test");
        // Replace this test with one or more real 'npipe' tests when we
        // implement 'npipe' support
        TestRecorder recorder;
        PythonProcessLauncher py("'npipe' test",
                                 "print 'Hello world'\n");
        py.mParams.files.add(LLProcess::FileParam());
        py.mParams.files.add(LLProcess::FileParam());
        py.mParams.files.add(LLProcess::FileParam("npipe"));
        py.mPy = LLProcess::create(py.mParams);
        ensure("should have rejected 'npipe'", ! py.mPy);
        std::string message(recorder.messageWith("npipe"));
        ensure("did not log 'npipe' type", ! message.empty());
        ensure_contains("did not name 'stderr'", message, "stderr");
    }

    template<> template<>
    void object::test<14>()
    {
        set_test_name("internal pipe name warning");
        TestRecorder recorder;
        PythonProcessLauncher py("pipe warning",
                                 "import sys\n"
                                 "sys.exit(7)\n");
        py.mParams.files.add(LLProcess::FileParam("pipe", "somename"));
        py.run();                   // verify that it did launch anyway
        ensure_equals("Status.mState", py.mPy->getStatus().mState, LLProcess::EXITED);
        ensure_equals("Status.mData",  py.mPy->getStatus().mData,  7);
        std::string message(recorder.messageWith("not yet supported"));
        ensure("did not log pipe name warning", ! message.empty());
        ensure_contains("log message did not mention internal pipe name",
                        message, "somename");
    }

#define TEST_getPipe(PROCESS, GETPIPE, GETOPTPIPE, VALID, NOPIPE, BADPIPE) \
    do                                                                  \
    {                                                                   \
        std::string threw;                                              \
        /* Both the following calls should work. */                     \
        (PROCESS).GETPIPE(VALID);                                       \
        ensure(#GETOPTPIPE "(" #VALID ") failed", (PROCESS).GETOPTPIPE(VALID)); \
        /* pass obviously bogus PIPESLOT */                             \
        CATCH_IN(threw, LLProcess::NoPipe, (PROCESS).GETPIPE(LLProcess::FILESLOT(4))); \
        ensure_contains("didn't reject bad slot", threw, "no slot");    \
        ensure_contains("didn't mention bad slot num", threw, "4");     \
        EXPECT_FAIL_WITH_LOG(threw, (PROCESS).GETOPTPIPE(LLProcess::FILESLOT(4))); \
        /* pass NOPIPE */                                               \
        CATCH_IN(threw, LLProcess::NoPipe, (PROCESS).GETPIPE(NOPIPE));  \
        ensure_contains("didn't reject non-pipe", threw, "not a monitored"); \
        EXPECT_FAIL_WITH_LOG(threw, (PROCESS).GETOPTPIPE(NOPIPE));      \
        /* pass BADPIPE: FILESLOT isn't empty but wrong direction */    \
        CATCH_IN(threw, LLProcess::NoPipe, (PROCESS).GETPIPE(BADPIPE)); \
        /* sneaky: GETPIPE is getReadPipe or getWritePipe */            \
        /* so skip "get" to obtain ReadPipe or WritePipe  :-P  */       \
        ensure_contains("didn't reject wrong pipe", threw, (#GETPIPE)+3); \
        EXPECT_FAIL_WITH_LOG(threw, (PROCESS).GETOPTPIPE(BADPIPE));     \
    } while (0)

/// For expecting exceptions. Execute CODE, catch EXCEPTION, store its what()
/// in std::string THREW, ensure it's not empty (i.e. EXCEPTION did happen).
#define CATCH_IN(THREW, EXCEPTION, CODE)                                \
    do                                                                  \
    {                                                                   \
        (THREW).clear();                                                \
        try                                                             \
        {                                                               \
            CODE;                                                       \
        }                                                               \
        catch (const EXCEPTION& e)                                      \
        {                                                               \
            (THREW) = e.what();                                         \
        }                                                               \
        ensure("failed to throw " #EXCEPTION ": " #CODE, ! (THREW).empty()); \
    } while (0)

#define EXPECT_FAIL_WITH_LOG(EXPECT, CODE)                              \
    do                                                                  \
    {                                                                   \
        TestRecorder recorder;                                          \
        ensure(#CODE " succeeded", ! (CODE));                           \
        ensure("wrong log message", ! recorder.messageWith(EXPECT).empty()); \
    } while (0)

    template<> template<>
    void object::test<15>()
    {
        set_test_name("get*Pipe() validation");
        PythonProcessLauncher py("just stderr",
                                 "print 'this output is expected'\n");
        py.mParams.files.add(LLProcess::FileParam("pipe")); // pipe for  stdin
        py.mParams.files.add(LLProcess::FileParam());       // inherit stdout
        py.mParams.files.add(LLProcess::FileParam("pipe")); // pipe for stderr
        py.run();
        TEST_getPipe(*py.mPy, getWritePipe, getOptWritePipe,
                     LLProcess::STDIN,   // VALID
                     LLProcess::STDOUT,  // NOPIPE
                     LLProcess::STDERR); // BADPIPE
        TEST_getPipe(*py.mPy, getReadPipe,  getOptReadPipe,
                     LLProcess::STDERR,  // VALID
                     LLProcess::STDOUT,  // NOPIPE
                     LLProcess::STDIN);  // BADPIPE
    }

    // TODO:
    // test pipe for stdin, stdout (etc.)
    // test getWritePipe().get_ostream(), getReadPipe().get_istream()
    // test listening on getReadPipe().getPump(), disconnecting
    // test setLimit(), getLimit()
    // test EOF -- check logging

} // namespace tut
