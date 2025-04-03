/*************************************************************************\
* PyDevice is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution.
\*************************************************************************/

#include "pywrapper.h"
#include "util.h"

#include <Python.h>

#include <map>
#include <stdexcept>
#include <iostream>
#include <vector>
#include <string>

static PyObject* globDict = nullptr;
static PyObject* locDict = nullptr;
static PyThreadState* mainThread = nullptr;
static std::map<std::string, std::pair<PyWrapper::Callback, PyObject*>> params;

struct PyGIL
{
    PyGILState_STATE state;
    PyGIL() {
        state = PyGILState_Ensure();
    }
    ~PyGIL() {
        PyGILState_Release(state);
    }
};

PyWrapper::ByteCode::ByteCode()
    : code(nullptr)
    , do_clear(false)
{
}

PyWrapper::ByteCode::ByteCode(void* c, bool _do_clear)
{
    code = c;
    do_clear = _do_clear;
}

PyWrapper::ByteCode::~ByteCode()
{
    assert(code == nullptr);
}

PyWrapper::ByteCode::ByteCode(ByteCode&& o)
{
    code = o.code;
    do_clear = o.do_clear;
    o.code = nullptr;
}

bool PyWrapper::ByteCode::clear_pyobjects() const
{
    return this->do_clear;
}

PyWrapper::ByteCode& PyWrapper::ByteCode::operator=(ByteCode&& o)
{
    code = o.code;
    do_clear = o.do_clear;
    o.code = nullptr;
    return *this;
}

/**
 * Function for caching parameter value or notifying record of new value.
 *
 * This function has two use-cases and are combined into a single function
 * for the simplicity to the end user:
 * 1) when Python code wants to send new parameter value to the record,
 *    it specifies two arguments, parameter name and its value.
 *    The function will cache the value and notify record that is has new
 *    value for the given parameter.
 * 2) Record(s) associated with that parameter name will then process and
 *    invoke this function with a single argument, in which case the cached
 *    parameter value is returned.
 */
static PyObject* pydev_iointr(PyObject* self, PyObject* args)
{
    PyObject* param;
    PyObject* value = nullptr;
    if (!PyArg_UnpackTuple(args, "pydev.iointr", 1, 2, &param, &value)) {
        PyErr_Clear();
        Py_RETURN_FALSE;
    }
#if PY_MAJOR_VERSION < 3
    if (!PyString_Check(param)) {
        PyErr_SetString(PyExc_TypeError, "Parameter name is not a string");
        Py_RETURN_NONE;
    }
    std::string name = PyString_AsString(param);
#else /* PY_MAJOR_VERSION < 3 */
    PyObject* tmp = nullptr;
    if (!PyUnicode_Check(param)){
        PyErr_SetString(PyExc_TypeError, "Parameter name is not a unicode");
        Py_RETURN_NONE;
    }
    tmp = PyUnicode_AsASCIIString(param);
    if(!tmp){
        PyErr_Clear();
        PyErr_SetString(PyExc_TypeError, "Unicode could not be converted to ASCII");
        Py_RETURN_NONE;
    }
    if(!PyBytes_Check(tmp)){
        PyErr_SetString(PyExc_TypeError, "PyUnicode as ASCII did not return expected bytes object!");
        Py_RETURN_NONE;
    }
    std::string name = PyBytes_AsString(tmp);
    Py_XDECREF(tmp);
#endif /* PY_MAJOR_VERSION < 3 */

    auto it = params.find(name);
    if (value) {
        if (it != params.end()) {
            if (it->second.second) {
                Py_DecRef(it->second.second);
            }
            Py_IncRef(value);
            it->second.second = value;

            it->second.first();
        }
        Py_RETURN_TRUE;
    }

    if (it != params.end() && it->second.second != nullptr) {
        Py_IncRef(it->second.second);
        return it->second.second;
    }
    Py_RETURN_NONE;
}

static struct PyMethodDef methods[] = {
    { "iointr", pydev_iointr, METH_VARARGS, "PyDevice interface for parameters exchange"},
    /* sentinel */
    { NULL, NULL, 0, NULL }
};

#if PY_MAJOR_VERSION < 3
static void PyInit_pydev(void)
{
    Py_InitModule("pydev", methods);
}
#else
static struct PyModuleDef moddef = {
    PyModuleDef_HEAD_INIT, "pydev", NULL, -1, methods, NULL, NULL, NULL, NULL
};
static PyObject* PyInit_pydev(void)
{
    return PyModule_Create(&moddef);
}
#endif

bool PyWrapper::init()
{
    // Initialize and register `pydev' Python module which serves as
    // communication channel for I/O Intr value exchange
    PyImport_AppendInittab("pydev", &PyInit_pydev);

    Py_InitializeEx(0);

#if PY_MAJOR_VERSION < 3
    PyEval_InitThreads();
    auto m = PyImport_AddModule("__main__");
    assert(m);
    globDict = PyModule_GetDict(m);
    locDict = PyDict_New();
#else /* PY_MAJOR_VERSION < 3 */

#if PY_MINOR_VERSION <= 6
    PyEval_InitThreads();
#endif

    globDict = PyDict_New();
    locDict = PyDict_New();
    PyDict_SetItemString(globDict, "__builtins__", PyEval_GetBuiltins());
#endif /* PY_MAJOR_VERSION < 3 */

    assert(globDict);
    assert(locDict);

    // Release GIL, save thread state
    mainThread = PyEval_SaveThread();

    // Make `pydev' module appear as built-in module
    exec("import pydev", true);

#if PY_MAJOR_VERSION < 3
    exec("import __builtin__", true);
    exec("__builtin__.pydev=pydev", true);
#else /* PY_MAJOR_VERSION < 3 */
    exec("import builtins", true);
    exec("builtins.pydev=pydev", true);
#endif /* PY_MAJOR_VERSION  <  3 */

    return true;
}

void PyWrapper::shutdown()
{
    PyEval_RestoreThread(mainThread);
    mainThread = nullptr;

    Py_DecRef(globDict);
    Py_DecRef(locDict);
    Py_Finalize();
}

void PyWrapper::registerIoIntr(const std::string& name, const Callback& cb)
{
    params[name].first = cb;
    params[name].second = nullptr;
}

bool PyWrapper::convert(void* in_, Variant& out)
{
    PyObject* in = reinterpret_cast<PyObject*>(in_);

#if PY_MAJOR_VERSION < 3
    if (PyString_Check(in)) {
        const char* o = PyString_AsString(in);
        if (o == nullptr && PyErr_Occurred()) {
            PyErr_Clear();
            return false;
        }
        out = Variant(o);
        return true;
    }
#endif
    if (PyUnicode_Check(in)) {
        PyObject* tmp = PyUnicode_AsASCIIString(in);
        if (tmp == nullptr) {
            PyErr_Clear();
            return false;
        }
        const char* o = PyBytes_AsString(tmp);
        if (o == nullptr && PyErr_Occurred()) {
            Py_XDECREF(tmp);
            PyErr_Clear();
            return false;
        }
        out = Variant(std::string(o));
        Py_XDECREF(tmp);
        return true;
    }
    if (PyBool_Check(in)) {
        out = Variant(static_cast<bool>(PyObject_IsTrue(in)));
        return true;
    }
#if PY_MAJOR_VERSION < 3
    if (PyInt_Check(in)) {
        auto o = PyInt_AsLong(in);
        if (o == -1 && PyErr_Occurred()) {
            PyErr_Clear();
            return false;
        }
        out = Variant(o);
        return true;
    }
#endif
    if (PyLong_Check(in)) {
        auto o = PyLong_AsLongLong(in);
        if (o == -1 && PyErr_Occurred()) {
            PyErr_Clear();
            return false;
        }
        out = Variant(o);
        return true;
    }
    if (PyFloat_Check(in)) {
        auto o = PyFloat_AsDouble(in);
        if (o == -1.0 && PyErr_Occurred()) {
            PyErr_Clear();
            return false;
        }
        out = Variant(o);
        return true;
    }

    if (PyList_Check(in)) {
        std::vector<double> vd;
        std::vector<long long int> vl;
        std::vector<std::string> vs;
        Variant::Type t = Variant::Type::NONE;

        for (Py_ssize_t i = 0; i < PyList_Size(in); i++) {
            PyObject* el = PyList_GetItem(in, i);
#if PY_MAJOR_VERSION < 3
            if (PyInt_Check(el) && (t == Variant::Type::NONE || t == Variant::Type::VECTOR_LONG)) {
                long long val = PyInt_AsLong(el);
                if (val == -1 && PyErr_Occurred()) {
                    PyErr_Clear();
                    return false;
                }
                vl.push_back(val);
                t = Variant::Type::VECTOR_LONG;
            }
#endif
            if (PyLong_Check(el) && (t == Variant::Type::NONE || t == Variant::Type::VECTOR_LONG)) {
                long val = PyLong_AsLong(el);
                if (val == -1 && PyErr_Occurred()) {
                    PyErr_Clear();
                    return false;
                }
                vl.push_back(val);
                t = Variant::Type::VECTOR_LONG;
            }
            if (PyBool_Check(el) && (t == Variant::Type::NONE || t == Variant::Type::VECTOR_LONG)) {
                long val = (PyObject_IsTrue(el) ? 1 : 0);
                vl.push_back(val);
                t = Variant::Type::VECTOR_LONG;
            }
            if (PyFloat_Check(el) && (t == Variant::Type::NONE || t == Variant::Type::VECTOR_DOUBLE)) {
                double val = PyFloat_AsDouble(el);
                if (val == -1.0 && PyErr_Occurred()) {
                    PyErr_Clear();
                    return false;
                }
                vd.push_back(val);
                t = Variant::Type::VECTOR_DOUBLE;
            }
#if PY_MAJOR_VERSION < 3
            if (PyString_Check(el) && (t == Variant::Type::NONE || t == Variant::Type::VECTOR_STRING)) {
                const char *cval = PyString_AsString(el);
                 PyObject *tmp = NULL; /* for symmetry with python3 code*/
#else
            if (PyUnicode_Check(el) && (t == Variant::Type::NONE || t == Variant::Type::VECTOR_STRING)) {
                PyObject* tmp = PyUnicode_AsASCIIString(el);
                if(!tmp){
                        PyErr_Clear();
                        return false;
                }
                const char* cval = PyBytes_AsString(tmp);
#endif
                if (!cval) {
                    Py_XDECREF(tmp);  // Free tmp before returning
                    PyErr_Clear();
                    return false;
                }
                vs.push_back(cval);
                t = Variant::Type::VECTOR_STRING;
            }
	}
        if (t == Variant::Type::VECTOR_LONG) {
            out = Variant(vl);
        } else if (t == Variant::Type::VECTOR_STRING) {
            out = Variant(vs);
        } else {
            out = Variant(vd);
        }

        return true;
    }

    // We don't support this type
    return false;
}

PyWrapper::ByteCode PyWrapper::compile(const std::string& code, bool debug)
{
    PyGIL gil;
    bool do_clear = true;
    PyObject* bytecode = Py_CompileString(code.c_str(), "", Py_eval_input);
    if (bytecode == NULL) {
        // Ignore error, try with Py_file_input which works for 'import xxx' etc.
        PyErr_Clear();
	do_clear = false;
        bytecode = Py_CompileString((code+"\n").c_str(), "", Py_file_input);
        if (bytecode == NULL) {
            if (debug) {
                PyErr_Print();
            }
            PyErr_Clear();
            //printf("Throwing error\n");
            throw SyntaxError();
        }
    }
    // std::cerr << "Clearing code for " << code << "? " << do_clear << std::endl;
    return ByteCode(bytecode, do_clear);
}

Variant PyWrapper::eval(const PyWrapper::ByteCode& bytecode, const std::map<std::string, Variant>& args, bool debug)
{
    PyGIL gil;

    std::vector<std::string> names_to_clear;

    for (auto& keyval: args) {
        PyObject* item = nullptr;
        if (keyval.second.type == Variant::Type::BOOL) {
            item = (keyval.second.get_bool() == true ? Py_True : Py_False);
        } else if (keyval.second.type == Variant::Type::LONG) {
            item = PyLong_FromLongLong(keyval.second.get_long());
        } else if (keyval.second.type == Variant::Type::UNSIGNED) {
            item = PyLong_FromUnsignedLongLong(keyval.second.get_unsigned());
        } else if (keyval.second.type == Variant::Type::DOUBLE) {
            item = PyFloat_FromDouble(keyval.second.get_double());
        } else if (keyval.second.type == Variant::Type::STRING) {
#if PY_MAJOR_VERSION < 3
            item = PyString_FromString(keyval.second.get_string().c_str());
#else
            item = PyUnicode_FromString(keyval.second.get_string().c_str());
#endif
        } else if (keyval.second.type == Variant::Type::VECTOR_LONG) {
            item = PyList_New(0);
            auto vals = keyval.second.get_long_array();
            for (auto& val: vals) {
                PyObject* element = PyLong_FromLongLong(val);
                assert(element);
                if(PyList_Append(item, element) == 0){
                    Py_DECREF(element);
                } else {
                    assert(0);
                }
            }
        } else if (keyval.second.type == Variant::Type::VECTOR_UNSIGNED) {
            item = PyList_New(0);
            auto vals = keyval.second.get_unsigned_array();
            for (auto& val: vals) {
                PyObject* element = PyLong_FromUnsignedLongLong(val);
                assert(element);
                if(PyList_Append(item, element) == 0){
                    Py_DECREF(element);
                } else {
                    assert(0);
                }
            }
        } else if (keyval.second.type == Variant::Type::VECTOR_DOUBLE) {
            item = PyList_New(0);
            auto vals = keyval.second.get_double_array();
            for (auto& val: vals) {
                PyObject* element = PyFloat_FromDouble(val);
                assert(element);
                if(PyList_Append(item, element) == 0){
                    Py_DECREF(element);
                } else {
                    assert(0);
                };
            }
        } else if (keyval.second.type == Variant::Type::VECTOR_STRING) {
            item = PyList_New(0);
            auto vals = keyval.second.get_string_array();
            for (auto& val: vals) {
#if PY_MAJOR_VERSION < 3
                PyObject* element = PyString_FromString(val.c_str());
#else
                PyObject* element = PyUnicode_FromString(val.c_str());
#endif
                assert(element);
                if(PyList_Append(item, element) == 0){
                    Py_DECREF(element);
                } else {
                    assert (0);
                }
            }
        }
        if (item == nullptr) {
            throw ArgumentError();
        }
        if(PyDict_SetItemString(locDict, keyval.first.c_str(), item) == 0){
	    names_to_clear.push_back(keyval.first);
	} else {
	    std::cerr << "Failed to add entry " << keyval.first << std::endl;
	}
        if (item != Py_True && item != Py_False) {
            Py_XDECREF(item);
        }
    }

#if PY_MAJOR_VERSION < 3
    auto code = reinterpret_cast<PyCodeObject*>(bytecode.code);
#else
    auto code = reinterpret_cast<PyObject*>(bytecode.code);
#endif
    if (code == nullptr)
    {
        throw std::invalid_argument("Missing compiled code");
    }

    PyObject *r = PyEval_EvalCode(code, globDict, locDict);
    if (bytecode.clear_pyobjects()){
	// std::cerr << "Clearing names "<< std::endl;
	    for(const auto& key: names_to_clear){
		if(PyDict_DelItemString(locDict, key.c_str()) == 0){
		    //  std::cerr << "Removed key " << key << " from local " << std::endl;
		} else if(PyDict_DelItemString(globDict, key.c_str()) == 0) {
		    //  std::cerr << "Removed key " << key << " from global " << std::endl;
		} else {
		    std::cerr << "Failed removed key " << key << " from local or global " << std::endl;
		}
	    }
    } else {
	if (names_to_clear.size()) {
	    std::cerr << __FILE__ << "::" << __LINE__
		      << " not clearing names: [";
	    for(const auto&  name: names_to_clear){
		std::cerr << " " << name;
	    }
	    std::cerr<< "]"<<std::endl;
	}
    }
    if (r == nullptr) {
        if (debug) {
            PyErr_Print();
        }
        PyErr_Clear();
        throw EvalError();
    }

    Variant val;
    bool converted = convert(r, val);
    if (!converted) {
        if (debug) {
            PyErr_Print();
        }
        PyErr_Clear();
    }
    Py_DecRef(r);

    return val;
}

Variant PyWrapper::exec(const std::string &code, const std::map<std::string, Variant> &args, bool debug)
{
    auto bytecode = std::move(compile(code, true));
    auto r = eval(bytecode, args, true);
    destroy(std::move(bytecode));
    return r;
}

void PyWrapper::destroy(PyWrapper::ByteCode&& bytecode)
{
    PyGIL gil;
    Py_XDECREF(reinterpret_cast<PyObject *>(bytecode.code));
    bytecode.code = nullptr;
}
