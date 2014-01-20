#include <stdexcept>
#include <QMessageBox>
#include <QFile>
#include <QDebug>
#include <QTime>

#include "interpreter.h"
#include "disconnectevent.h"
#include "videowidget.h"
#include "console.h"
#include "mainwindow.h"
#include "renderer.h"
#include "calc.h"

QString printType(uint32_t val, bool parens=false);

Interpreter::Interpreter(ConsoleWidget *console, VideoWidget *video, MainWindow *main) : m_mutexProg(QMutex::Recursive)
{
    m_console = console;
    m_video = video;
    m_main = main;
    m_pc = 0;
    m_programming = false;
    m_localProgramRunning = false;
    m_rcount = 0;
    m_waiting = false;
    m_fastPoll = true;
    m_pendingCommand = NONE;
    m_running = -1; // set to bogus value to force update
    m_chirp = NULL;


#if 0
    ChirpProc proc, procGet, procGetInfo, procGetAll;
    uint8_t buf[0x40];
    proc = m_chirp->getProc("prm_set");
    procGet = m_chirp->getProc("prm_get");
    procGetInfo = m_chirp->getProc("prm_getInfo");
    procGetAll = m_chirp->getProc("prm_getAll");

    uint32_t dead, baad;
    uint16_t beef;
    uint8_t ab;
    uint8_t *buf2;
    char *desc;
    char *id;
    int response, len, res;
    len = m_chirp->serialize(false, buf, 0x40, UINT32(0xa1b2c3d4), UINT16(0xc1ab), UINT8(0xca), UINT32(0xea12345), END);
    res = m_chirp->callSync(proc, STRING("hello"), UINTS8(len, buf), END_OUT_ARGS, &response, END_IN_ARGS);
    res = m_chirp->callSync(procGet, STRING("hello"), END_OUT_ARGS, &response, &len, &buf2, END_IN_ARGS);
    res = Chirp::deserialize(buf2, len, &dead, &beef, &ab, &baad, END);
    res = m_chirp->callSync(procGetInfo, STRING("hello"), END_OUT_ARGS, &response, &desc, END_IN_ARGS);
    res = m_chirp->callSync(procGetAll, UINT16(0), END_OUT_ARGS, &response, &id, &desc, &len, &buf2, END_IN_ARGS);
    res = m_chirp->callSync(procGetAll, UINT16(1), END_OUT_ARGS, &response, &id, &desc, &len, &buf2, END_IN_ARGS);
    res = m_chirp->callSync(procGetAll, UINT16(2), END_OUT_ARGS, &response, &id, &desc, &len, &buf2, END_IN_ARGS);
    res = m_chirp->callSync(procGetAll, UINT16(3), END_OUT_ARGS, &response, &id, &desc, &len, &buf2, END_IN_ARGS);

#endif

    m_renderer = new Renderer(m_video);

    connect(m_console, SIGNAL(textLine(QString)), this, SLOT(command(QString)));
    connect(m_console, SIGNAL(controlKey(Qt::Key)), this, SLOT(controlKey(Qt::Key)));
    connect(this, SIGNAL(textOut(QString, QColor)), m_console, SLOT(print(QString, QColor)));
    connect(this, SIGNAL(error(QString)), m_console, SLOT(error(QString)));
    connect(this, SIGNAL(enableConsole(bool)), m_console, SLOT(acceptInput(bool)));
    connect(this, SIGNAL(prompt(QString)), m_console, SLOT(prompt(QString)));
    connect(this, SIGNAL(videoInput(VideoWidget::InputMode)), m_video, SLOT(acceptInput(VideoWidget::InputMode)));
    connect(m_video, SIGNAL(selection(int,int,int,int)), this, SLOT(handleSelection(int,int,int,int)));
    // we necessarily want to execute in the gui thread, so queue
    connect(this, SIGNAL(connected(Device,bool)), m_main, SLOT(handleConnected(Device,bool)), Qt::QueuedConnection);

    m_run = true;
    start();
}

Interpreter::~Interpreter()
{
    m_localProgramRunning = false;
    m_console->m_mutexPrint.lock();
    m_console->m_waitPrint.wakeAll();
    m_console->m_mutexPrint.unlock();
    m_waitInput.wakeAll();

    m_run = false;
    wait();
    clearLocalProgram();
    if (m_chirp)
        delete m_chirp;
}

int Interpreter::execute()
{
    int res;

    emit runState(true);
    emit enableConsole(false);

    QMutexLocker locker(&m_mutexProg);

    while(1)
    {
        for (; m_pc<m_program.size(); m_pc++)
        {
            if (!m_localProgramRunning)
            {
                prompt();
                res = 0;
                goto end;
            }
            res = m_chirp->execute(m_program[m_pc]);
            if (res<0)
                goto end;
        }
        m_pc = 0;
    }
end:
    m_localProgramRunning = false;
    emit runState(false);
    emit enableConsole(true);

    return res;
}


QString Interpreter::printArgType(uint8_t *type, int &index)
{
    if (*type==CRP_INT8)
        return "INT8";
    else if (*type==CRP_INT16)
        return "INT16";
    else if (*type==CRP_INT32)
        return "INT32";
    else if (*type==CRP_TYPE_HINT)
    {
        int n = strlen((char *)type);
        QString print = "HINT(";
        if (n>4)
        {
            print += printType(FOURCC(type[1], type[2], type[3], type[4]), false) + ") ";
            index += 4;
        }
        else
        {
            index += n-1;
            print += "error)";
        }
        return print;
    }
    else
        return "?";
}

QString Interpreter::printProc(const ProcInfo *info, int level)
{
    ArgList list;
    QString print;
    QStringList sections;
    int i;

    print = QString(info->procName) + "(";
    if (getArgs(info, &list)<0)
        return "";
    for (i=0; i<(int)list.size(); i++)
    {
        if (i>0)
            print +=  ", ";
        print += printArgType(&info->argTypes[i], i) + " " + list[i].first;
    }
    print += ")\n";

    if (level>0)
    {
        sections = getSections("", info->procInfo);
        if (sections.size()>0)
            print += sections[0] + "\n";
        print += "Parameters:\n";
        for (i=0; i<(int)list.size(); i++)
        {
            print += "   " + list[i].first + ": ";
            print += list[i].second + "\n";
        }
        sections = getSections("@r", info->procInfo);
        print += "Returns:\n";
        for (i=0; i<sections.size(); i++)
            print += "   " + sections[i] + "\n";
    }

    return print;
}

void Interpreter::printHelp()
{
    ProcInfo info;
    ChirpProc p;

    for (p=0; true; p++)
    {
        if (m_chirp->getProcInfo(p, &info)<0)
            break;
        emit textOut(QString::number(p) + ": " + printProc(&info));
    }
}

QStringList Interpreter::getSections(const QString &id, const QString &string)
{
    int i;
    QStringList sections, words;
    QString section;

    if(id=="")
    {
        sections << string.section('@', 0, 0);
        return sections;
    }

    for (i=1; true; i++)
    {
        section = string.section('@', i, i, QString::SectionIncludeLeadingSep);
        if (section=="@")
            break;
        words = section.split(QRegExp("\\s+"));
        if (words[0].contains(id))
        {
            section.remove(words[0]); // remove id
            section.remove(QRegExp("^\\s+")); // remove leading whitespace
            sections << section;
        }
    }

    return sections;
}

int Interpreter::getArgs(const ProcInfo *info, ArgList *argList)
{
    int i;
    QString label, desc, procInfo(info->procInfo);
    QStringList sections, words;

    if (info->argTypes==NULL)
        return -1;

    sections = getSections("@p", procInfo);
    for (i=0; info->argTypes[i]; i++)
    {
        if (i<sections.size())
        {
            words = sections[i].split(QRegExp("\\s+"));
            label = words[0];
            desc = sections[i];
            desc.remove(QRegExp("^\\s+")); // remove leading whitespace
            desc.remove(QRegExp("^" + label)); // remove id
            desc.remove(QRegExp("^\\s+")); // remove leading whitespace
            argList->push_back(Arg(label, desc));
            // if argument is a type hint, we need to skip the 4-byte (32-bit) int, error if we run out of args
            if (info->argTypes[i]==CRP_TYPE_HINT &&
                    (!info->argTypes[++i] || !info->argTypes[++i] || !info->argTypes[++i] || !info->argTypes[++i]))
                return -1;
        }
        else
            argList->push_back(Arg(QString("arg")+QString::number(i), ""));
    }
    return 0;
}


QString printType(uint32_t val, bool parens)
{
    QString res;
    QChar a, b, c, d;
    uint32_t val2 = val;

    a = (QChar)(val2&0xff);
    val2 >>= 8;
    b = (QChar)(val2&0xff);
    val2 >>= 8;
    c = (QChar)(val2&0xff);
    val2 >>= 8;
    d = (QChar)(val2&0xff);

    if (a.isPrint() && b.isPrint() && c.isPrint() && d.isPrint())
    {
        if (parens)
            res = QString("FOURCC(") + a + b + c + d + ")";
        else
            res = QString(a) + b + c + d;
    }
    else
    {
        if (parens)
            res = "HTYPE(0x" + QString::number((uint)val, 16) + ")";
        else
            res = "0x" + QString::number((uint)val, 16);
    }

    return res;
}

void Interpreter::handleResponse(void *args[])
{
    // strip off response, add to print string
    m_print = "response " + QString::number(m_rcount++) + ": " +
            QString::number(*(int *)args[0]) + " (0x" + QString::number((uint)*(uint *)args[0], 16) + ") ";

    // render rest of response, if present
    handleData(args+1);
}

void Interpreter::handleData(void *args[])
{
    uint8_t type;
    QColor color = CW_DEFAULT_COLOR;

    if (args[0])
    {
        type = Chirp::getType(args[0]);
        if (type==CRP_TYPE_HINT)
        {
            m_print += printType(*(uint32_t *)args[0]) + " frame data\n";
            m_renderer->render(*(uint32_t *)args[0], args+1);
        }
        else if (type==CRP_HSTRING)
        {
            m_print +=  (char *)args[0];
            color = Qt::blue;
        }
        else
            qDebug() << "unknown type " << type;
    }
    if (m_print.right(1)!="\n")
        m_print += "\n";

    // wait queue business keeps worker thread from getting too far ahead of gui thread
    // (when this happens, things can get sluggish.)
    if (m_localProgramRunning || m_running)
        m_console->m_mutexPrint.lock();
    emit textOut(m_print, color);
    m_print = "";
    if (m_localProgramRunning || m_running)
    {
        m_console->m_waitPrint.wait(&m_console->m_mutexPrint);
        m_console->m_mutexPrint.unlock();
    }
}

int Interpreter::addProgram(ChirpCallData data)
{
    QMutexLocker locker(&m_mutexProg);

    m_program.push_back(data);

    return 0;
}

int Interpreter::addProgram(const QStringList &argv)
{
    QMutexLocker locker(&m_mutexProg);

    m_programText.push_back(argv);

    return 0;
}

#if 0
bool Interpreter::checkRemoteProgram()
{
    int res;
    bool result;

    res = getRunning();
    if (res<0)
        return false;

    result = (bool)res;

    emit runState(result);
    emit enableConsole(!result);

    return result;
}

int Interpreter::stopRemoteProgram()
{
    int i, res;

    qDebug() << "stopremoteprogram";
    res = sendStop();
    if (res<0)
        return -1;

    // poll for 500ms for program to stop
    for (i=0; i<10; i++)
    {
        res = getRunning();
        if (res<0)
            return -1;
        if (res==false)
        {
            emit runState(false);
            emit enableConsole(true);
            qDebug() << "success";
            return 0;
        }
        msleep(50);
    }
    qDebug() << "error";
    return -1;
}
#endif

void Interpreter::getRunning()
{
    int res, running;

    res = m_chirp->callSync(m_exec_running, END_OUT_ARGS, &running, END_IN_ARGS);
    if (res<0)
    {
        running = false;
        emit connected(PIXY, false);
    }
    // emit state if we've changed
    if (m_running!=running)
    {
        m_fastPoll = false;
        m_running = running;
        emit runState(running);
        emit enableConsole(!running);
        if (!running)
            prompt();
    }
}

int Interpreter::sendRun()
{
    int res, response;

    m_fastPoll = true;

    res = m_chirp->callSync(m_exec_run, END_OUT_ARGS, &response, END_IN_ARGS);
    if (res<0)
        return res;
    return response;
}

int Interpreter::sendStop()
{
    int res, response;

    m_fastPoll = true;

    res = m_chirp->callSync(m_exec_stop, END_OUT_ARGS, &response, END_IN_ARGS);
    if (res<0)
        return res;
    return response;
}


#if 0
void Interpreter::run()
{
    int res;

begin:
    if (m_init)
    {
        try
        {
            if (m_link.open()<0)
                throw std::runtime_error("Unable to open USB device.");
            m_chirp = new ChirpMon(this, &m_link);
            m_exec_run = m_chirp->getProc("run");
            m_exec_running = m_chirp->getProc("running");
            m_exec_stop = m_chirp->getProc("stop");
            if (m_exec_run<0 || m_exec_running<0 || m_exec_stop<0)
                throw std::runtime_error("Communication error with Pixy.");
            m_disconnect = new DisconnectEvent(this);
        }
        catch (std::runtime_error &exception)
        {
            emit error(QString(exception.what()));
            return;
        }
        m_remoteProgramRunning = checkRemoteProgram(); // get initial state (is program running or not?)
        m_init = false;
        qDebug() << "*** init done";
    }

    if (m_setModel)
        setModel();
    else if (m_remoteProgramRunning)
    {
        if (!getRunning()) // if we're not running, we should start
            sendRun();

        emit runState(true);
        emit enableConsole(false);

        while(m_remoteProgramRunning)
        {
            m_chirp->m_mutex.lock();
            m_chirp->service(false);
            m_chirp->m_mutex.unlock();
        }
        qDebug() << "stopping...";
        if (!m_exit)  // if we're being destructed we shouldn't stop the remote program or print the prompt
        {
            stopRemoteProgram();
            prompt();
        }
    }
    else if (m_localProgramRunning)
    {
        emit runState(true);
        emit enableConsole(false);

        res = execute();

        emit runState(false);
        emit enableConsole(true);

        // check for cable disconnect
        if (res) //==LIBUSB_ERROR_PIPE)
        {
            emit connected(PIXY, false);
            return;
        }
    }
    else
    {
        res = call(m_argv, true);

        if (res<0 && m_programming)
        {
            endLocalProgram();
            clearLocalProgram();
        }
        prompt();
        // check to see if we're running after this command-- if so, go back
        if (!m_programming)
        {
            m_remoteProgramRunning = checkRemoteProgram(); // get state
            if (m_remoteProgramRunning)
                goto begin; // I know......
        }
    }
}
#else

void Interpreter::handlePendingCommand()
{
    switch (m_pendingCommand)
    {
    case NONE:
        break;

    case STOP:
        sendStop();
        break;

    case RUN:
        sendRun();
        break;
    }
    m_pendingCommand = NONE;
}

void Interpreter::run()
{
    int res;
    QTime time;

    // init
    try
    {
        if (m_link.open()<0)
            throw std::runtime_error("Unable to open USB device.");
        m_chirp = new ChirpMon(this, &m_link);
        m_exec_run = m_chirp->getProc("run");
        m_exec_running = m_chirp->getProc("running");
        m_exec_stop = m_chirp->getProc("stop");
        if (m_exec_run<0 || m_exec_running<0 || m_exec_stop<0)
            throw std::runtime_error("Communication error with Pixy.");
    }
    catch (std::runtime_error &exception)
    {
        emit error(QString(exception.what()));
        return;
    }
    qDebug() << "*** init done";

    time.start();
    getRunning();

    while(m_run)
    {
        if (!m_programming &&
                ((m_fastPoll && time.elapsed()>RUN_POLL_PERIOD_FAST) ||
                (!m_fastPoll && time.elapsed()>RUN_POLL_PERIOD_SLOW)))
        {
            getRunning();
            time.restart();
        }
        else
            m_chirp->service(false);

        handlePendingCommand();
        if (!m_running)
        {
            if (m_localProgramRunning)
                execute();
            else
            {
                if (m_mutexProg.tryLock())
                {
                    if (m_argv.size())
                    {
                        if (m_argv[0]=="help")
                            handleHelp();
                        else
                        {
                            res = call(m_argv, true);
                            if (res<0 && m_programming)
                            {
                                endLocalProgram();
                                clearLocalProgram();
                            }
                        }
                        m_argv.clear();
                        prompt();
                        // check quickly to see if we're running after this command
                        if (!m_programming)
                            getRunning();
                    }
                    m_mutexProg.unlock();
                }
            }
        }
    }
}

#endif

int Interpreter::beginLocalProgram()
{
    if (m_programming)
        return -1;
    m_programming = true;
    return 0;
}

int Interpreter::endLocalProgram()
{
    if (!m_programming)
        return -1;
    m_pc = 0;
    m_programming = false;
    return 0;
}

int Interpreter::runLocalProgram()
{
    QMutexLocker locker(&m_mutexProg);

    if (m_localProgramRunning || m_program.size()==0)
        return -1;

    m_console->emptyLine(); // don't want to start printing on line with prompt

    m_localProgramRunning = true;

    return 0;
}

void Interpreter::runOrStopProgram()
{
    if (m_localProgramRunning)
        m_localProgramRunning = false;
    else if (m_running==false)
        m_pendingCommand = RUN;
    else if (m_running==true)
        m_pendingCommand = STOP;
    // no case to run local program because this is sort of an undocumented feature
}

uint Interpreter::programRunning()
{
    if (m_localProgramRunning)
        return m_localProgramRunning;
    if (m_running>0)
        return m_running;
    return false;
}


int Interpreter::clearLocalProgram()
{
    QMutexLocker locker(&m_mutexProg);
    unsigned int i;

    for (i=0; i<m_program.size() && m_localProgramRunning; i++)
    {
        ChirpCallData data = m_program[i];
        delete [] data.m_buf;
    }
    m_program.clear();
    m_programText.clear();

    return 0;
}

void Interpreter::listProgram()
{
    QMutexLocker locker(&m_mutexProg);
    QString print;
    unsigned int i;
    int j;

    for (i=0; i<m_programText.size(); i++)
    {
        if (m_programText[i].size()<1)
            continue;

        print += QString::number(i+1) + ": " + m_programText[i][0] + " ";
        for (j=1; j<m_programText[i].size(); j++)
        {
            if (j>1)
                print += ", ";
            print += m_programText[i][j];
        }
        print += "\n";
     }

    emit textOut(print);
}

void Interpreter::prompt()
{
    if (m_programming)
        emit prompt("prog" + QString::number(m_program.size()+1) + PROMPT);
    else
        emit prompt(PROMPT);
}

void Interpreter::command(const QString &command)
{
    if (m_localProgramRunning)
        return;

    if (m_waiting)
    {
        QString command2 = command;
        command2.remove(QRegExp("[(),\\t]"));
        m_command = command2;
        m_key = (Qt::Key)0;
        m_waitInput.wakeAll();
        return;
    }

    QStringList words = command.split(QRegExp("[\\s(),\\t]"), QString::SkipEmptyParts);

    if (words.size()==0)
        goto end;

    if (words[0]=="do")
    {
        clearLocalProgram();
        beginLocalProgram();
    }
    else if (words[0]=="done")
    {
        endLocalProgram();
        runLocalProgram();
        return;
    }
    else if (words[0]=="list")
        listProgram();
    else if (words[0].left(4)=="cont")
    {
        if (runLocalProgram()>=0)
            return;
    }
    else if (words[0]=="rendermode")
    {
        if (words.size()>1)
            m_renderer->setMode(words[1].toInt());
        else
            emit textOut("Missing mode parameter.\n");
    }
    else if (words[0]=="set")
    {
        if (words.size()==3)
        {
            words[1].remove(QRegExp("[\\s\\D]+"));
            m_renderer->m_blobs.setLabel(words[1], words[2]);
        }
    }
    else
    {
        handleCall(words);
        return; // don't print prompt
    }

end:
    prompt();
}

void Interpreter::controlKey(Qt::Key key)
{
    m_command = "";
    m_key = key;
    m_waitInput.wakeAll();
    if (m_programming)
        endLocalProgram();
    prompt();

}


void Interpreter::handleHelp()
{
    ChirpProc proc;
    ProcInfo info;

    if (m_argv.size()==1)
        printHelp();
    else if (m_argv.size()>1)
    {
        if ((proc=m_chirp->getProc(m_argv[1].toLocal8Bit()))>=0 && m_chirp->getProcInfo(proc, &info)>=0)
            emit textOut(printProc(&info, 1));
        else
            emit error("can't find procedure.\n");
    }
}


#if 0
int Interpreter::call(const QString &command)
{
    int res;
    QStringList argv = command.split(QRegExp("[\\s(),\\t]"), QString::SkipEmptyParts);

    if (argv.size()==0)
        res = -1;
    else
        res = call(argv);

    if (res<0 && m_programming)
    {
        endLocalProgram();
        clearLocalProgram();
    }

    return res;
}

#endif

void Interpreter::handleCall(const QStringList &argv)
{
    m_mutexProg.lock();
    m_argv = argv;
    m_mutexProg.unlock();
}

#if 0
int Interpreter::uploadLut()
{
    uint32_t i, sum;
    uint32_t responseInt;

    for (i=0, sum=0; i<LUT_SIZE; i++)
        sum += m_lut[i];
    qDebug() << sum;
    ChirpProc setmem = m_chirp->getProc("cc_setMemory");
    for (i=0; i<LUT_SIZE; i+=0x100)
        m_chirp->callSync(setmem, UINT32(0x10082000+i), UINTS8(0x100, m_lut+i), END_OUT_ARGS, &responseInt, END_IN_ARGS);

    return 0;
}
#endif

void Interpreter::handleSelection(int x0, int y0, int width, int height)
{
    m_command = QString::number(x0) + " " + QString::number(y0) +  " " + QString::number(width) +  " " + QString::number(height);
    m_key = (Qt::Key)0;
    m_waitInput.wakeAll();
}

int Interpreter::call(const QStringList &argv, bool interactive)
{
    ChirpProc proc;
    ProcInfo info;
    int args[20];
    int i, j, n, base, res;
    bool ok;
    uint type;
    ArgList list;

    // not allowed
    if (argv.size()<1)
        return -1;

    // a procedure needs extension info (arg info, etc) in order for us to call...
    if ((proc=m_chirp->getProc(argv[0].toLocal8Bit()))>=0 &&
            m_chirp->getProcInfo(proc, &info)>=0)
    {
        memset(args, 0, sizeof(args)); // zero args
        getArgs(&info, &list);
        n = strlen((char *)info.argTypes);

        // if we have fewer args than required...
        if ((int)list.size()>argv.size()-1)
        {
            // if we're interactive, ask for values
            if (interactive && argv.size()>0)
            {
                QStringList cargv = argv;
                QString pstring, pstring2;
                for (i=cargv.size()-1; i<(int)list.size(); i++)
                {
                    if (info.argTypes[i]==CRP_TYPE_HINT)
                    {
                        if (n>i+4)
                        {
                            type = *(uint *)&info.argTypes[i+1];
                            if (type==FOURCC('R','E','G','1'))
                            {
                                emit videoInput(VideoWidget::REGION);
                                pstring2 = "(select region with mouse)";
                            }
                            if (type==FOURCC('P','N','T','1'))
                            {
                                emit videoInput(VideoWidget::POINT);
                                pstring2 = "(select point with mouse)";
                            }

                            emit enableConsole(false);
                        }

                    }
                    pstring = printArgType(&info.argTypes[i], i) + " " + list[i].first +
                            (list[i].second=="" ? "?" : " (" + list[i].second + ")?") + " " + pstring2;

                    emit prompt(pstring);
                    m_mutexInput.lock();
                    m_waiting = true;
                    m_waitInput.wait(&m_mutexInput);
                    m_waiting = false;
                    m_mutexInput.unlock();

                    emit enableConsole(true);

                    if (m_key==Qt::Key_Escape)
                        return -1;
                    cargv << m_command.split(QRegExp("\\s+"));
                }
                // call ourselves again, now that we have all the args
                return call(cargv, true);
            }
            else
            {
                emit error("too few arguments.\n");
                return -1;
            }
        }


        augmentProcInfo(&info);
        // if we have all the args we need, parse, put in args array
        for (i=0, j=0; m_argTypes[i]; i++)
        {
            if (argv.size()>i+1)
            {
                if (m_argTypes[i]==CRP_INT8 || m_argTypes[i]==CRP_INT16 || m_argTypes[i]==CRP_INT32)
                {
                    args[j++] = m_argTypes[i];
                    if (argv[i+1].left(2)=="0x")
                        base = 16;
                    else
                        base = 10;
                    args[j++] = argv[i+1].toInt(&ok, base);
                    if (!ok)
                    {
                        emit error("argument didn't parse.\n");
                        return -1;
                    }
                }
#if 0
                else if (m_argTypes[i]==CRP_STRING)
                {
                    args[j++] = m_argTypes[i];
                    // string goes where?  can't cast pointer to int...
                }
#endif
                else
                {
                    // deal with non-integer types
                    return -1;
                }
            }
        }
#if 0
        // print helpful chirp argument string
        if (interactive && argv.size()>1)
        {
            QString callString = "Chirp arguments for " + argv[0] +
                    " (ChirpProc=" + QString::number(proc) + "): ";
            for (i=1; i<argv.size(); i++)
            {
                if (i>1)
                    callString += ", ";
                callString += printArgType(&m_argTypes[i-1], i) + "(" + argv[i] + ")";
            }
            emit textOut(callString + "\n");
        }
#endif

        // make chirp call
        res = m_chirp->callAsync(proc, args[0], args[1], args[2], args[3], args[4], args[5], args[6],
                           args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14], args[15],
                           args[16], args[17], args[18], args[19], END_OUT_ARGS);

        // check for cable disconnect
        if (res<0) //res==LIBUSB_ERROR_PIPE)
        {
            emit connected(PIXY, false);
            return res;
        }
        // get response if we're not programming, save text if we are
        if (m_programming)
            addProgram(argv);
        else
            m_chirp->serviceChirp();
    }
    else
    {
        emit error("procedure unsupported.\n");
        return -1;
    }

    return 0;
}

void Interpreter::augmentProcInfo(ProcInfo *info)
{
    int i, n;
    uint type;
    uint8_t types[0x100];

    n = strlen((char *)info->argTypes);
    memcpy(m_argTypes, info->argTypes, n+1);
    for (i=0; m_argTypes[i]; i++)
    {
        if (m_argTypes[i]==CRP_TYPE_HINT)
        {
            if (n>i+4)
            {
                type = *(uint *)&m_argTypes[i+1];
                memcpy(types, &m_argTypes[i+5], n-(i+5)+1);
                if (type==FOURCC('R','E','G','1'))
                {
                    m_argTypes[i++] = CRP_UINT16;
                    m_argTypes[i++] = CRP_UINT16;
                    m_argTypes[i++] = CRP_UINT16;
                    m_argTypes[i++] = CRP_UINT16;
                }
                else if (type==FOURCC('P','N','T','1'))
                {
                    m_argTypes[i++] = CRP_UINT16;
                    m_argTypes[i++] = CRP_UINT16;
                }
                memcpy(&m_argTypes[i], types, strlen((char *)types)+1);
                i += strlen((char *)types);
            }
        }
    }
}

