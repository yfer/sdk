// Copyright (c) 2017, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:async';

/// [Try] is similar to Haskell monad, where
/// a computation may throw an exception.
/// There is no checking of passing null into
/// the value, so be mindful.
class Try<T> {
  final T _val;
  final StackTrace _stackTrace;
  final dynamic _err;

  Try.from(this._val)
      : _err = null,
        _stackTrace = null;

  Try.fail(this._err, this._stackTrace) : _val = null;

  Try<S> bind<S>(S f(T x)) {
    if (_err != null) {
      return new Try.fail(_err, _stackTrace);
    }
    try {
      return new Try<S>.from(f(_val));
    } catch (ex, stackTrace) {
      return new Try<S>.fail(ex, stackTrace);
    }
  }

  Future<Try<S>> bindAsync<S>(Future<S> f(T x)) async {
    if (_err != null) {
      return new Try.fail(_err, _stackTrace);
    }
    try {
      return new Try.from(await f(_val));
    } catch (ex, stackTrace) {
      return new Try.fail(ex, stackTrace);
    }
  }

  void fold(void caseErr(dynamic ex, StackTrace st), void caseVal(T x)) {
    if (_err != null) {
      caseErr(_err, _stackTrace);
    } else {
      caseVal(_val);
    }
  }

  Future foldAsync(
      Future caseErr(dynamic ex, StackTrace st), Future caseVal(T x)) async {
    if (_err != null) {
      await caseErr(_err, _stackTrace);
    } else {
      await caseVal(_val);
    }
  }

  bool get isError => _err != null;

  Exception get error => _err;
  StackTrace get stackTrace => _stackTrace;

  T get value => _val;

  T getOrDefault(T defval) => _err != null ? defval : _val;
}

Try<T> tryStart<T>(T action()) {
  return new Try<int>.from(0).bind<T>((dummy) => action());
}

Future<Try<T>> tryStartAsync<T>(Future<T> action()) {
  return new Try<int>.from(0).bindAsync<T>((dummy) => action());
}
