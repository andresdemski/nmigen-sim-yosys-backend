#include <iostream>
#include <vector>
#include <Python.h>
#include <unistd.h>
#include <stdio.h>

enum TRIGGERS {OTHER=0, TIMER, EDGE, R_EDGE, F_EDGE};

template <size_t Bits> static void set_next_value(void *signal, PyObject *v);
template <size_t Bits> static PyObject * get_current_value(void *signal);

std::vector<struct task_handler_t> main_tasks;
std::vector<struct task_handler_t> forked_tasks;

#ifdef DEBUG
    #define DEBUG_PRINT(...) do{ fprintf( stderr, __VA_ARGS__ ); } while( false )
#else
    #define DEBUG_PRINT(...) do{} while( false )
#endif

#define WIDTH_TO_CHUNKS( x ) (( ( x ) + 32 - 1) / 32)

cxxrtl_design::p_top top;
uint32_t sim_time = 0;

struct signal_handler_t {
    const char * name;
    uint32_t width;
    uint32_t chunks;
    uint32_t *curr;
    uint32_t *next;
};

struct signal_status_t {
    uint32_t signal;
    uint32_t chunks;
    uint32_t start_time;
    uint32_t * curr;
    uint32_t * prev;
};

struct signal_handler_t handler_list[] = {
    {.name = "rst", .width = 1,  .chunks = WIDTH_TO_CHUNKS(1),  .curr = top.p_rst.curr.data, .next = top.p_rst.next.data},
    {.name = "clk", .width = 1,  .chunks = WIDTH_TO_CHUNKS(1),  .curr = top.p_clk.curr.data, .next = top.p_clk.next.data},
    {.name = "r",   .width = 66, .chunks = WIDTH_TO_CHUNKS(66), .curr = top.p_r.curr.data,   .next = top.p_r.next.data},
    {.name = "b",   .width = 65, .chunks = WIDTH_TO_CHUNKS(65), .curr = top.p_b.curr.data,   .next = top.p_b.next.data},
    {.name = "a",   .width = 65, .chunks = WIDTH_TO_CHUNKS(65), .curr = top.p_a.curr.data,   .next = top.p_a.next.data},
};

static bool timer_trigger(void * data) {
    if ((*(long *) data) == sim_time) {
        return true;
    }
    return false;
}

uint32_t n_of_chunks(uint32_t width) {
    return (width + 32 - 1) / 32;
}

static void copy_chunks(uint32_t *orig, uint32_t *dest, uint32_t chunks) {
    for (int i=0; i < chunks; i++) {
        dest[i] = orig[i];
    }
}

static bool is_zero(uint32_t *data, uint32_t chunks) {
    for (int i=0; i < chunks; i++) {
        if (data[i] != 0) return false;
    }
    return true;
}

static bool other_trigger(void * data) {
    return true;
}

static bool edge_trigger(void * data) {
    signal_status_t * p = (signal_status_t*) data;
    uint32_t * current = handler_list[p->signal].curr;
    bool rv = false;
    if (p->start_time != sim_time) {
        for (int i=0; i < p->chunks; i++) {
            rv |= p->prev[i] != current[i];
        }
    }
    copy_chunks(current, p->prev, p->chunks);
#ifdef DEBUG
    if(rv) DEBUG_PRINT("@%d: EDGE\n", sim_time);
#endif
    return rv;
}

static bool redge_trigger(void * data) {
    signal_status_t * p = (signal_status_t*) data;
    uint32_t * current = handler_list[p->signal].curr;
    bool rv = true;
    if (p->start_time == sim_time) rv = false;
    if (not is_zero(p->prev, p->chunks)) rv = false;
    if (is_zero(current, p->chunks)) rv = false;
    copy_chunks(current, p->prev, p->chunks);
#ifdef DEBUG
    if (rv) DEBUG_PRINT("@%d: RISING EDGE\n", sim_time);
#endif
    return rv;
}

static bool fedge_trigger(void * data) {
    signal_status_t * p = (signal_status_t*) data;
    uint32_t * current = handler_list[p->signal].curr;
    bool rv = true;
    if (p->start_time == sim_time) rv = false;
    if (is_zero(p->prev, p->chunks)) rv = false;
    if (not is_zero(current, p->chunks)) rv = false;
    copy_chunks(current, p->prev, p->chunks);
#ifdef DEBUG
    if (rv) DEBUG_PRINT("@%d: RISING EDGE\n", sim_time);
#endif
    return rv;
}

bool (*TRIGGER_LIST[])(void*) = {
    other_trigger,
    timer_trigger,
    edge_trigger,
    redge_trigger,
    fedge_trigger
};

struct task_handler_t {
    PyObject *coro;
    bool (*trigger)(void *);
    void * data;
    long type;

    void alloc_trigger(long trigger_type) {
#ifdef DEBUG
        DEBUG_PRINT("@%d: Alloc trigger\n", sim_time);
#endif
        if (trigger_type == TIMER) data = (void*) new long;
        else data = (void*) new signal_status_t;
    }

    void release_trigger() {
#ifdef DEBUG
        DEBUG_PRINT("@%d: Release trigger\n", sim_time);
#endif
        free(data);
        trigger = NULL;
        data = NULL;
    }

    void add_trigger(long trigger_type, long value) {
        signal_status_t * p;
        uint32_t chunks;

#ifdef DEBUG
        DEBUG_PRINT("@%d: Adding trigger\n", sim_time);
#endif

        if (trigger && type > TIMER) delete ((signal_status_t*)data)->prev;
        if (trigger && type != trigger_type) release_trigger();
        if (trigger == NULL) alloc_trigger(trigger_type);

        type = trigger_type;
        switch (type) {
            case TIMER:
                *((long*) data) = sim_time + value;
                break;
            case EDGE:
            case R_EDGE:
            case F_EDGE:
                p = (signal_status_t*) data;
                p->signal = value;
                p->chunks = n_of_chunks(handler_list[value].width);
                p->start_time = sim_time;
                p->curr = handler_list[value].curr;
                p->prev = new uint32_t[p->chunks];
                for (int i = 0; i < p->chunks; i++) p->prev[i] = p->curr[i];
                break;
        }
        trigger = TRIGGER_LIST[type];
    }
};

static void pylong_to_chunks(PyObject *v, uint32_t *data, uint32_t chunks) {
    PyLongObject * l = (PyLongObject*) v;
    long ob_size = Py_SIZE(l);
    uint64_t buf = 0;
    int buffered = 0;

    unsigned int i = 0, j = 0;
    for (unsigned int i = 0; i < chunks; i++) data[i] = 0;

    if (ob_size != 0) {
        for (unsigned int i = 0; i < ob_size; i++) {
            buf |= (((uint64_t) (l->ob_digit[i] & 0x3fffffff)) << buffered);
            buffered += 30;
            while(buffered >= 32) {
                data[j++] = buf & 0xffffffff;
                buf = buf >> 32;
                buffered -= 32;
            }
        }
        if (buffered != 0) data[j] = buf & 0xffffffff;;
    }
}

static PyObject * chunks_to_pylong(uint32_t *data, uint32_t chunks) {
    const uint32_t max_ob_size = (chunks * 32 + 30 - 1) / 30 + 2;
    uint64_t buf = 0;
    uint32_t buffered = 0;
    uint32_t ob_digit[max_ob_size];
    uint32_t digits = 0;
    uint32_t ob_size = max_ob_size - 1;

    if (is_zero(data, chunks)) return PyLong_FromLong(0L);
    for (unsigned int i = 0; i < max_ob_size; i++) ob_digit[i] = 0;
    for (unsigned int i = 0; i < chunks; i++) {
        buf |= ((uint64_t) data[i]) << buffered;
        buffered += 32;
        while(buffered >= 30) {
            ob_digit[digits++] = buf & 0x3fffffff;
            buf = buf >> 30;
            buffered -= 30;
        }
    }
    if (buffered != 0) ob_digit[digits++] = buf;
    while(ob_size > 0 && ob_digit[ob_size] == 0) ob_size--;

    PyLongObject * l = _PyLong_New(ob_size+ 1);
    for (unsigned int i = 0; i <= ob_size; i++) l->ob_digit[i] = ob_digit[i];
    return (PyObject *) l;
}

static PyObject* get_signal_name(PyObject *self, PyObject *args) {
    uint32_t id;
    PyArg_ParseTuple(args,"i", &id);
    return Py_BuildValue("s", handler_list[id].name);
}

static PyObject* get_signal_width(PyObject *self, PyObject *args) {
    uint32_t id;
    PyArg_ParseTuple(args,"i", &id);
    return Py_BuildValue("s", handler_list[id].width);
}

static PyObject* set_by_id(PyObject *self, PyObject *args) {
    uint32_t id;
    PyObject * v;
    PyArg_ParseTuple(args,"iO", &id, &v);
    pylong_to_chunks(v, handler_list[id].next, handler_list[id].chunks);
    Py_RETURN_NONE;
}

static PyObject* get_by_id(PyObject *self, PyObject *args) {
    uint32_t id;
    PyArg_ParseTuple(args,"i", &id);
    return chunks_to_pylong(handler_list[id].curr, handler_list[id].chunks);
}

static void step() {
	do {
		top.eval();
	} while (top.commit());
    sim_time++;
}

static PyObject* pystep(PyObject *self, PyObject *args) {
    step();
    Py_RETURN_NONE;
}

static PyObject* n_of_signals(PyObject *self, PyObject *args) {
    return Py_BuildValue("i", sizeof(handler_list)/sizeof(struct signal_handler_t));
}

static PyObject* get_sim_time(PyObject *self, PyObject *args) {
    return Py_BuildValue("i", sim_time);
}

static PyObject* add_task(PyObject *self, PyObject *args) {
    PyObject * gen;
    PyArg_ParseTuple(args,"O", &gen);
    main_tasks.push_back(task_handler_t{gen, NULL, NULL, 0});
    Py_RETURN_NONE;
}

static PyObject* fork(PyObject *self, PyObject *args) {
    PyObject * coro;
    PyArg_ParseTuple(args,"O", &coro);

    auto ret = PyObject_CallMethod(coro, "__next__", "");
    if (PyErr_ExceptionMatches(PyExc_StopIteration)) {
        PyErr_Clear();
        Py_RETURN_NONE;
    }
    if (PyErr_Occurred()) Py_RETURN_NONE;

    auto trigger = PyLong_AsLong(PyTuple_GET_ITEM(ret, 0));
    auto data = PyLong_AsLong(PyTuple_GET_ITEM(ret, 1));

    task_handler_t task = task_handler_t{coro, NULL, NULL};

    task.add_trigger(trigger, data);
    forked_tasks.push_back(task);
    Py_RETURN_NONE;
}

static PyObject* scheduller(PyObject *self, PyObject *args) {
    PyObject *ret, *error;
    long trigger, data;
    
    while (true) {
        do {
            for (auto it=main_tasks.begin(); it != main_tasks.end();) {
                if (not it->trigger or it->trigger(it->data)) {
                    ret = PyObject_CallMethod(it->coro, "__next__", "");
                    if (PyErr_ExceptionMatches(PyExc_StopIteration)) Py_RETURN_NONE;
                    error = PyErr_Occurred();
                    if (error) Py_RETURN_NONE;
                    trigger = PyLong_AsLong(PyTuple_GET_ITEM(ret, 0));
                    data = PyLong_AsLong(PyTuple_GET_ITEM(ret, 1));
                    it->add_trigger(trigger, data);
                }
                it++;
            }
            
            if (forked_tasks.size() != 0) {
                for (auto it=forked_tasks.begin(); it != forked_tasks.end();) {
                    if (not it->trigger or it->trigger(it->data)) {
                        ret = PyObject_CallMethod(it->coro, "__next__", "");
                        if (PyErr_ExceptionMatches(PyExc_StopIteration)) {
                            if (it->data) {free(it->data); it->data = NULL;}
                            forked_tasks.erase(it);
                            PyErr_Clear();
                        }
                        error = PyErr_Occurred();
                        if (error) Py_RETURN_NONE;
                        trigger = PyLong_AsLong(PyTuple_GET_ITEM(ret, 0));
                        data = PyLong_AsLong(PyTuple_GET_ITEM(ret, 1));
                        it->add_trigger(trigger, data);
                        it++;
                    }
                    else it++;
                }
            }
            top.eval();
        } while(top.commit());
        
        sim_time++;
        if (PyErr_CheckSignals()) Py_RETURN_NONE;
    }
    Py_RETURN_NONE;
}

static PyMethodDef simulator_methods[] = { 
    // Signal access
    {"n_of_signals", n_of_signals, METH_NOARGS, "delta step"},
    {"get_signal_name", get_signal_name, METH_VARARGS, "set value by signal id"},
    {"set_by_id", set_by_id, METH_VARARGS, "set value by signal id"},
    {"get_by_id", get_by_id, METH_VARARGS, "get value by signal id"},

    // Time step
    {"step", pystep, METH_NOARGS, "time step"},

    // Scheduller
    {"fork", fork, METH_VARARGS, "add task to scheduller"},
    {"add_task", add_task, METH_VARARGS, "add task to scheduller"},
    {"scheduller", scheduller, METH_NOARGS, "add task to scheduller"},

    // Utils
    {"get_sim_time", get_sim_time, METH_NOARGS, "delta step"},

    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef simulation_definition = { 
    PyModuleDef_HEAD_INIT,
    "simulation",
    "yosys simulation in python",
    -1, 
    simulator_methods
};

PyMODINIT_FUNC PyInit_simulation(void) {
    Py_Initialize();
    return PyModule_Create(&simulation_definition);
}
