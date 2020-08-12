#define PY_SSIZE_T_CLEAN
#include <stdlib.h>
#include <Python.h>
#include <frameobject.h>
#include <time.h>
#include "snaptrace.h"

// We need to ignore the first event because it's return of start() function
int first_event = 1;
int collecting = 0;
unsigned int check_flags = 0;
int max_stack_depth = -1;
int curr_stack_depth = 0;
int ignore_stack_depth = 0;
PyObject* include_files = NULL;
PyObject* exclude_files = NULL;

struct FEENode {
    PyObject* file_name;
    PyObject* class_name;
    PyObject* func_name;
    int type;
    double ts;
    struct FEENode* next;
    struct FEENode* prev;
} *buffer_head, *buffer_tail;

static void Print_Py(PyObject* o)
{
    printf("%s\n", PyUnicode_AsUTF8(PyObject_Repr(o)));
}

int
snaptrace_tracefunc(PyObject* obj, PyFrameObject* frame, int what, PyObject* arg)
{
    if (what == PyTrace_CALL || what == PyTrace_RETURN) {
        struct FEENode* node = NULL;
        struct timespec t;

        if (first_event) {
            first_event = 0;
            return 0;
        }

        // Check max stack depth
        if (CHECK_FLAG(check_flags, SNAPTRACE_MAX_STACK_DEPTH)) {
            if (what == PyTrace_CALL) {
                curr_stack_depth += 1;
                if (curr_stack_depth > max_stack_depth) {
                    return 0;
                }
            } else if (what == PyTrace_RETURN) {
                curr_stack_depth -= 1;
                if (curr_stack_depth + 1 > max_stack_depth) {
                    return 0;
                }
            }
        }

        // Check include/exclude files
        if (CHECK_FLAG(check_flags, SNAPTRACE_INCLUDE_FILES | SNAPTRACE_EXCLUDE_FILES)) {
            if (what == PyTrace_CALL && ignore_stack_depth > 0) {
                ignore_stack_depth += 1;
                return 0;
            } else if (what == PyTrace_RETURN && ignore_stack_depth > 0) {
                ignore_stack_depth -= 1;
                return 0;
            }
            if (ignore_stack_depth == 0) {
                PyObject* files = NULL;
                int record = 0;
                int is_include = CHECK_FLAG(check_flags, SNAPTRACE_INCLUDE_FILES);
                if (is_include) {
                    files = include_files;
                    record = 0;
                } else {
                    files = exclude_files;
                    record = 1;
                }
                Py_ssize_t length = PyList_GET_SIZE(files);
                PyObject* name = frame->f_code->co_filename;
                char* path = realpath(PyUnicode_AsUTF8(name), NULL);
                if (path) {
                    for (int i = 0; i < length; i++) {
                        PyObject* f = PyList_GET_ITEM(files, i);
                        if (strstr(path, PyUnicode_AsUTF8(f))) {
                            record = 1 - record;
                            break;
                        }
                    }
                    free(path);
                }
                if (record == 0) {
                    ignore_stack_depth += 1;
                    return 0;
                }
            } else {
                return 0;
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &t);
        if (buffer_tail->next) {
            node = buffer_tail->next;
        } else {
            node = (struct FEENode*)PyMem_Malloc(sizeof(struct FEENode));
            node->next = NULL;
            buffer_tail->next = node;
            node->prev = buffer_tail;
        }
        node->file_name = frame->f_code->co_filename;
        Py_INCREF(node->file_name);
        node->class_name = Py_None;
        Py_INCREF(Py_None);
        for (int i = 0; i < frame->f_code->co_nlocals; i++) {
            PyObject* name = PyTuple_GET_ITEM(frame->f_code->co_varnames, i);
            if (strcmp("self", PyUnicode_AsUTF8(name)) == 0) {
                // When self object is just created in __new__, it's possible that the value is NULL
                if (frame->f_localsplus[i]) {
                    node->class_name = PyUnicode_FromString(frame->f_localsplus[i]->ob_type->tp_name);
                    Py_DECREF(Py_None);
                }
                break;
            }
        }
        node->func_name = frame->f_code->co_name;
        Py_INCREF(node->func_name);
        node->type = what;
        node->ts = ((double)t.tv_sec * 1e9 + t.tv_nsec);
        buffer_tail = node;
    }


    return 0;
}

static PyObject*
snaptrace_start(PyObject* self, PyObject* args)
{
    PyEval_SetProfile(snaptrace_tracefunc, NULL);
    first_event = 1;
    collecting = 1;
    curr_stack_depth = 0;
    ignore_stack_depth = 0;

    Py_RETURN_NONE;
}

static PyObject*
snaptrace_stop(PyObject* self, PyObject* args)
{
    PyEval_SetProfile(NULL, NULL);
    if (collecting == 1) {
        // If we are collecting, throw away the last event
        // if it's an entry. because it's entry of stop() function
        struct FEENode* node = buffer_tail;
        if (node->type == PyTrace_CALL) {
            Py_DECREF(node->file_name);
            Py_DECREF(node->class_name);
            Py_DECREF(node->func_name);
            buffer_tail = buffer_tail->prev;
            collecting = 0;
        }
    }
    
    Py_RETURN_NONE;
}

static PyObject*
snaptrace_load(PyObject* self, PyObject* args)
{
    PyObject* lst = PyList_New(0);
    struct FEENode* curr = buffer_head;
    while (curr != buffer_tail && curr->next) {
        struct FEENode* node = curr->next;
        PyObject* tuple = PyTuple_Pack(5, PyLong_FromLong(node->type), PyFloat_FromDouble(node->ts) ,node->file_name, node->class_name, node->func_name);
        PyList_Append(lst, tuple);
        curr = curr->next;
    }
    buffer_tail = buffer_head;
    return lst;
}

static PyObject*
snaptrace_clear(PyObject* self, PyObject* args)
{
    struct FEENode* curr = buffer_head;
    while (curr != buffer_tail && curr->next) {
        struct FEENode* node = curr->next;
        Py_DECREF(node->file_name);
        Py_DECREF(node->class_name);
        Py_DECREF(node->func_name);
        curr = curr->next;
    }
    buffer_tail = buffer_head;

    Py_RETURN_NONE;
}

static PyObject* 
snaptrace_cleanup(PyObject* self, PyObject* args)
{
    snaptrace_clear(self, args);
    while (buffer_head->next) {
        struct FEENode* node = buffer_head->next;
        buffer_head->next = node->next;
        PyMem_FREE(node);
    } 
    Py_RETURN_NONE;
}

static PyObject*
snaptrace_config(PyObject* self, PyObject* args, PyObject* kw)
{
    static char* kwlist[] = {"max_stack_depth", "include_files", "exclude_files"};
    int kw_max_stack_depth = 0;
    PyObject* kw_include_files = NULL;
    PyObject* kw_exclude_files = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "|iOO", kwlist, 
            &kw_max_stack_depth,
            &kw_include_files,
            &kw_exclude_files)) {
        return NULL;
    }

    check_flags = 0;
    if (kw_max_stack_depth >= 0) {
        SET_FLAG(check_flags, SNAPTRACE_MAX_STACK_DEPTH);       
        max_stack_depth = kw_max_stack_depth;
    } else {
        UNSET_FLAG(check_flags, SNAPTRACE_MAX_STACK_DEPTH);
    }

    if (kw_include_files && kw_include_files != Py_None) {
        if (include_files) {
            Py_DECREF(include_files);
        }
        include_files = kw_include_files;
        Py_INCREF(include_files);
        SET_FLAG(check_flags, SNAPTRACE_INCLUDE_FILES);
    } else {
        UNSET_FLAG(check_flags, SNAPTRACE_INCLUDE_FILES);
    }

    if (kw_exclude_files && kw_exclude_files != Py_None) {
        if (exclude_files) {
            Py_DECREF(exclude_files);
        }
        exclude_files = kw_exclude_files;
        Py_INCREF(exclude_files);
        SET_FLAG(check_flags, SNAPTRACE_EXCLUDE_FILES);
    } else {
        UNSET_FLAG(check_flags, SNAPTRACE_EXCLUDE_FILES);
    }

    Py_RETURN_NONE;
}

static PyMethodDef SnaptraceMethods[] = {
    {"start", snaptrace_start, METH_VARARGS, "start profiling"},
    {"stop", snaptrace_stop, METH_VARARGS, "stop profiling"},
    {"load", snaptrace_load, METH_VARARGS, "load buffer"},
    {"clear", snaptrace_clear, METH_VARARGS, "clear buffer"},
    {"cleanup", snaptrace_cleanup, METH_VARARGS, "free the memory allocated"},
    {"config", (PyCFunction)snaptrace_config, METH_VARARGS|METH_KEYWORDS, "config the snaptrace module"}
};

static struct PyModuleDef snaptracemodule = {
    PyModuleDef_HEAD_INIT,
    "codesnap.snaptrace",
    NULL,
    -1,
    SnaptraceMethods
};

PyMODINIT_FUNC
PyInit_snaptrace(void) 
{
    buffer_head = (struct FEENode*) PyMem_Malloc (sizeof(struct FEENode));
    buffer_head->class_name = NULL;
    buffer_head->file_name = NULL;
    buffer_head->func_name = NULL;
    buffer_head->next = NULL;
    buffer_head->prev = NULL;
    buffer_head->type = -1;
    buffer_tail = buffer_head; 
    first_event = 1;
    collecting = 0;
    return PyModule_Create(&snaptracemodule);
}