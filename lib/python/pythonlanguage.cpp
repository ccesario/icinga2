/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012 Icinga Development Team (http://www.icinga.org/)        *
 *                                                                            *
 * This program is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU General Public License                *
 * as published by the Free Software Foundation; either version 2             *
 * of the License, or (at your option) any later version.                     *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software Foundation     *
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
 ******************************************************************************/

#include "i2-python.h"

using namespace icinga;

PythonInterpreter *PythonLanguage::m_CurrentInterpreter;

REGISTER_SCRIPTLANGUAGE("Python", PythonLanguage);

PyMethodDef PythonLanguage::m_NativeMethodDef[] = {
	{ "RegisterFunction", &PythonLanguage::PyRegisterFunction, METH_VARARGS, NULL },
	{ NULL, NULL } /* sentinel */
};

PythonLanguage::PythonLanguage(void)
	: ScriptLanguage()
{
	Py_Initialize();
	PyEval_InitThreads();

	Py_SetProgramName(Application::GetArgV()[0]);
	PySys_SetArgv(Application::GetArgC(), Application::GetArgV());

	// See http://docs.python.org/2/c-api/init.html for an explanation.
	PyRun_SimpleString("import sys; sys.path.pop(0)\n");

	m_MainThreadState = PyThreadState_Get();

	m_NativeModule = Py_InitModule("ire", m_NativeMethodDef);

	(void) PyThreadState_Swap(NULL);
	PyEval_ReleaseLock();

	String name;
	ScriptFunction::Ptr function;
	BOOST_FOREACH(tie(name, function), ScriptFunction::GetFunctions()) {
		RegisterNativeFunction(name, function);
	}

	ScriptFunction::OnRegistered.connect(boost::bind(&PythonLanguage::RegisterNativeFunction, this, _1, _2));
	ScriptFunction::OnUnregistered.connect(boost::bind(&PythonLanguage::UnregisterNativeFunction, this, _1));
}

PythonLanguage::~PythonLanguage(void)
{
	/* Due to how we're destructing objects it might not be safe to
	 * call Py_Finalize() when the Icinga instance is being shut
	 * down - so don't bother calling it. */
}

ScriptInterpreter::Ptr PythonLanguage::CreateInterpreter(const Script::Ptr& script)
{
	return boost::make_shared<PythonInterpreter>(GetSelf(), script);
}

PyThreadState *PythonLanguage::GetMainThreadState(void) const
{
	return m_MainThreadState;
}

PyObject *PythonLanguage::MarshalToPython(const Value& value)
{
	String svalue;

	switch (value.GetType()) {
		case ValueEmpty:
			Py_INCREF(Py_None);
			return Py_None;

		case ValueNumber:
			return PyFloat_FromDouble(value);

		case ValueString:
			svalue = value;
			return PyString_FromString(svalue.CStr());

		case ValueObject:
			if (value.IsObjectType<DynamicObject>()) {
				DynamicObject::Ptr dobj = value;

				String type = dobj->GetType()->GetName();
				String name = dobj->GetName();

				PyObject *ptype = PyString_FromString(type.CStr());

				if (ptype == NULL)
					return NULL;

				PyObject *pname = PyString_FromString(name.CStr());

				if (pname == NULL) {
					Py_DECREF(ptype);

					return NULL;
				}

				PyObject *result = PyTuple_New(2);

				if (result == NULL) {
					Py_DECREF(ptype);
					Py_DECREF(pname);

					return NULL;
				}

				(void) PyTuple_SetItem(result, 0, ptype);
				(void) PyTuple_SetItem(result, 1, pname);

				return result;
			} else if (value.IsObjectType<Dictionary>()) {
				Dictionary::Ptr dict = value;
				PyObject *pdict = PyDict_New();

				String key;
				Value value;
				BOOST_FOREACH(tie(key, value), dict) {
					PyObject *dv = MarshalToPython(value);

					PyDict_SetItemString(pdict, key.CStr(), dv);
				}

				return pdict;
			}

			Py_INCREF(Py_None);
			return Py_None;

		default:
			BOOST_THROW_EXCEPTION(invalid_argument("Unexpected variant type."));
	}
}

Value PythonLanguage::MarshalFromPython(PyObject *value)
{
	if (value == Py_None) {
		return Empty;
	} else if (PyDict_Check(value)) {
		Dictionary::Ptr dict = boost::make_shared<Dictionary>();

		PyObject *dk, *dv;
		Py_ssize_t pos = 0;

		while (PyDict_Next(value, &pos, &dk, &dv)) {
		    String ik = PyString_AsString(dk);
		    Value iv = MarshalFromPython(dv);

		    dict->Set(ik, iv);
		}

		return dict;
	} else if (PyTuple_Check(value) && PyTuple_Size(value) == 2) {
		PyObject *ptype, *pname;

		ptype = PyTuple_GetItem(value, 0);

		if (ptype == NULL || !PyString_Check(ptype))
			BOOST_THROW_EXCEPTION(invalid_argument("Tuple must contain two strings."));

		String type = PyString_AsString(ptype);

		pname = PyTuple_GetItem(value, 1);

		if (pname == NULL || !PyString_Check(pname))
			BOOST_THROW_EXCEPTION(invalid_argument("Tuple must contain two strings."));

		String name = PyString_AsString(pname);

		DynamicObject::Ptr object = DynamicObject::GetObject(type, name);

		if (!object)
			BOOST_THROW_EXCEPTION(invalid_argument("Object '" + name + "' of type '" + type + "' does not exist."));

		return object;
	} else if (PyFloat_Check(value)) {
		return PyFloat_AsDouble(value);
	} else if (PyInt_Check(value)) {
		return PyInt_AsLong(value);
	} else if (PyString_Check(value)) {
		return PyString_AsString(value);
	} else {
		return Empty;
	}
}

PyObject *PythonLanguage::PyCallNativeFunction(PyObject *self, PyObject *args)
{
	assert(PyString_Check(self));

	char *name = PyString_AsString(self);

	ScriptFunction::Ptr function = ScriptFunction::GetByName(name);

	vector<Value> arguments;

	if (args != NULL) {
		if (PyTuple_Check(args)) {
			for (Py_ssize_t i = 0; i < PyTuple_Size(args); i++) {
				PyObject *arg = PyTuple_GetItem(args, i);

				arguments.push_back(MarshalFromPython(arg));
			}
		} else {
			arguments.push_back(MarshalFromPython(args));
		}
	}

	PyThreadState *tstate = PyEval_SaveThread();

	Value result;

	try {
		ScriptTask::Ptr task = boost::make_shared<ScriptTask>(function, arguments);
		task->Start();
		task->Wait();

		result = task->GetResult();
	} catch (const std::exception& ex) {
		PyEval_RestoreThread(tstate);

		String message = diagnostic_information(ex);
		PyErr_SetString(PyExc_RuntimeError, message.CStr());

		return NULL;
	}

	PyEval_RestoreThread(tstate);

	return MarshalToPython(result);
}

/**
 * Registers a native function.
 *
 * @param name The name of the native function.
 * @param function The function.
 */
void PythonLanguage::RegisterNativeFunction(const String& name, const ScriptFunction::Ptr& function)
{
	PyThreadState *tstate = PyThreadState_Swap(m_MainThreadState);

	PyObject *pname = PyString_FromString(name.CStr());

	PyMethodDef *md = new PyMethodDef;
	md->ml_name = strdup(name.CStr());
	md->ml_meth = &PythonLanguage::PyCallNativeFunction;
	md->ml_flags = METH_VARARGS;
	md->ml_doc = NULL;

	PyObject *pfunc = PyCFunction_NewEx(md, pname, m_NativeModule);
	(void) PyModule_AddObject(m_NativeModule, name.CStr(), pfunc);

	(void) PyThreadState_Swap(tstate);
}

/**
 * Unregisters a native function.
 *
 * @param name The name of the native function.
 */
void PythonLanguage::UnregisterNativeFunction(const String& name)
{
	PyThreadState *tstate = PyThreadState_Swap(m_MainThreadState);

	PyObject *pdict = PyModule_GetDict(m_NativeModule);
	PyObject *pname = PyString_FromString(name.CStr());
	PyCFunctionObject *pfunc = (PyCFunctionObject *)PyDict_GetItem(pdict, pname);

	if (pfunc && PyCFunction_Check(pfunc)) {
		/* Eww. */
		free(const_cast<char *>(pfunc->m_ml->ml_name));
		delete pfunc->m_ml;
	}

	(void) PyDict_DelItem(pdict, pname);
	Py_DECREF(pname);

	(void) PyThreadState_Swap(tstate);
}

PyObject *PythonLanguage::PyRegisterFunction(PyObject *self, PyObject *args)
{
	char *name;
	PyObject *object;

	if (!PyArg_ParseTuple(args, "sO", &name, &object))
		return NULL;

	PythonInterpreter *interp = GetCurrentInterpreter();

	if (interp == NULL) {
		PyErr_SetString(PyExc_RuntimeError, "GetCurrentInterpreter() returned NULL.");
		return NULL;
	}

	if (!PyCallable_Check(object)) {
		PyErr_SetString(PyExc_RuntimeError, "Function object is not callable.");
		return NULL;
	}

	{
		boost::mutex::scoped_lock lock(Application::GetMutex());
		interp->RegisterPythonFunction(name, object);
	}


	Py_INCREF(Py_None);
	return Py_None;
}

/**
 * Retrieves the current interpreter object. Caller must hold the GIL.
 *
 * @returns The current interpreter.
 */
PythonInterpreter *PythonLanguage::GetCurrentInterpreter(void)
{
	return m_CurrentInterpreter;
}

/**
 * Sets the current interpreter. Caller must hold the GIL.
 *
 * @param interpreter The interpreter.
 */
void PythonLanguage::SetCurrentInterpreter(PythonInterpreter *interpreter)
{
	m_CurrentInterpreter = interpreter;
}