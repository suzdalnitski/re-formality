module Validation = Formality__Validation;

module Strategy = Formality__Strategy;

module Utils = Formality__Utils;

let defaultDebounceInterval = 700;

module type Config = {
  type field;
  type state;
  type message;
  let get: (field, state) => Validation.value;
  let update: ((field, Validation.value), state) => state;
  let strategy: Strategy.t;
  type validators;
  let validators: validators;
  let debounceInterval: int;
  module Validators: {
    type t(+'a);
    let find: (field, t('debouncedValidator)) => 'debouncedValidator;
    let fold:
      ((field, 'debouncedValidator, 'a) => 'a, t('debouncedValidator), 'a) =>
      'a;
    let map:
      (
        Validation.asyncValidator(field, state, message) => 'debouncedValidator,
        validators
      ) =>
      t('debouncedValidator);
  };
};

module Make = (Form: Config) => {
  module FieldsSetOrigin =
    Set.Make(
      {
        type t = Form.field;
        let compare = Utils.comparator;
      }
    );
  module FieldsSet = {
    type t = FieldsSetOrigin.t;
    let empty = FieldsSetOrigin.empty;
    let isEmpty = FieldsSetOrigin.is_empty;
    let mem = FieldsSetOrigin.mem;
    let add = FieldsSetOrigin.add;
    let remove = FieldsSetOrigin.remove;
  };
  module ResultsMapOrigin =
    Map.Make(
      {
        type t = Form.field;
        let compare = Utils.comparator;
      }
    );
  module ResultsMap = {
    type key = ResultsMapOrigin.key;
    type t = ResultsMapOrigin.t(option(Validation.result(Form.message)));
    let empty = ResultsMapOrigin.empty;
    let add = ResultsMapOrigin.add;
    let get = (key: key, map: t) =>
      switch (map |> ResultsMapOrigin.find(key)) {
      | result => result
      | exception Not_found => None
      };
  };
  exception NoResultInResultsMapOnSubmit(Form.field);
  type state = {
    data: Form.state,
    results: ResultsMap.t,
    validating: FieldsSet.t,
    submitting: bool,
    submittedOnce: bool,
    emittedFields: FieldsSet.t
  };
  type action =
    | Change((Form.field, Validation.value))
    | Blur((Form.field, Validation.value))
    | InvokeDebouncedAsyncValidation(
        Form.field,
        Validation.value,
        (
          ~field: Form.field,
          ~value: Validation.value,
          ReasonReact.self(state, ReasonReact.noRetainedProps, action)
        ) =>
        unit
      )
    | TriggerAsyncValidation(
        Form.field,
        Validation.value,
        Validation.validateAsync(Form.message)
      )
    | ApplyAsyncResult(
        Form.field,
        Validation.value,
        Validation.result(Form.message)
      )
    | Submit
    | Reset
    | HandleSubmissionError;
  type interface = {
    state: Form.state,
    results: Form.field => option(Validation.result(Form.message)),
    validating: Form.field => bool,
    submitting: bool,
    change: (Form.field, ReactEventRe.Form.t) => unit,
    blur: (Form.field, ReactEventRe.Focus.t) => unit,
    submit: ReactEventRe.Form.t => unit
  };
  let getInitialState = data => {
    data,
    results: ResultsMap.empty,
    validating: FieldsSet.empty,
    submitting: false,
    submittedOnce: false,
    emittedFields: FieldsSet.empty
  };
  let debounce = (~validateAsync, ~wait) => {
    let lastSelf = ref(None);
    let lastField = ref(None);
    let lastValue = ref(None);
    let lastCallTime = ref(None);
    let timerId = ref(None);
    let shouldInvoke = time =>
      switch lastCallTime^ {
      | None => true
      | Some(lastCallTime) =>
        let timeSinceLastCall = time - lastCallTime;
        timeSinceLastCall >= wait || timeSinceLastCall < 0;
      };
    let remainingWait = time =>
      switch lastCallTime^ {
      | None => wait
      | Some(lastCallTime) =>
        let timeSinceLastCall = time - lastCallTime;
        wait - timeSinceLastCall;
      };
    let rec timerExpired = () => {
      let time = Js.Date.now() |> int_of_float;
      time |> shouldInvoke ?
        invoke() :
        timerId :=
          Some(Js.Global.setTimeout(timerExpired, time |> remainingWait));
    }
    and invoke = () => {
      timerId := None;
      if (lastValue^ |> Js.Option.isSome) {
        let field = lastField^;
        let value = lastValue^;
        let self = lastSelf^;
        lastSelf := None;
        lastValue := None;
        switch (field, value, self) {
        | (Some(field), Some(value), Some({ReasonReact.send})) =>
          send(TriggerAsyncValidation(field, value, validateAsync))
        | _ => ()
        };
      };
    };
    let debounced = (~field, ~value, self) => {
      let time = Js.Date.now() |> int_of_float;
      lastCallTime := Some(time);
      lastField := Some(field);
      lastValue := Some(value);
      lastSelf := Some(self);
      timerId := Some(Js.Global.setTimeout(timerExpired, wait));
    };
    debounced;
  };
  type debouncedValidator = {
    strategy: option(Strategy.t),
    dependents: option(list(Form.field)),
    validate: Validation.validate(Form.state, Form.message),
    validateAsync:
      option(
        (
          ~field: Form.field,
          ~value: Validation.value,
          ReasonReact.self(state, ReasonReact.noRetainedProps, action)
        ) =>
        unit
      )
  };
  let debouncedValidators =
    Form.validators
    |> Form.Validators.map(validator =>
         switch validator.validateAsync {
         | Some(validateAsync) => {
             strategy: validator.strategy,
             dependents: validator.dependents,
             validate: validator.validate,
             validateAsync:
               Some(debounce(~validateAsync, ~wait=Form.debounceInterval))
           }
         | None => {
             strategy: validator.strategy,
             dependents: validator.dependents,
             validate: validator.validate,
             validateAsync: None
           }
         }
       );
  let getValidator = field =>
    switch (debouncedValidators |> Form.Validators.find(field)) {
    | validator => Some(validator)
    | exception Not_found => None
    };
  let getStrategy = (validator: debouncedValidator) =>
    validator.strategy |> Js.Option.getWithDefault(Form.strategy);
  let validateDependents = (~data, ~results, ~emittedFields, dependents) =>
    dependents
    |> List.fold_left(
         ((results', emittedFields'), field') => {
           let validator = field' |> getValidator;
           let emitted = emittedFields |> FieldsSet.mem(field');
           switch (validator, emitted) {
           | (None, _)
           | (_, false) => (results', emittedFields')
           | (Some(validator), true) =>
             let result = data |> validator.validate(data |> Form.get(field'));
             (
               results' |> ResultsMap.add(field', Some(result)),
               emittedFields' |> FieldsSet.add(field')
             );
           };
         },
         (results, emittedFields)
       );
  let component = ReasonReact.reducerComponent("FormalityForm");
  let make =
      (
        ~initialState: Form.state,
        ~onSubmit: (Form.state, Validation.notifiers) => unit,
        children
      ) => {
    ...component,
    initialState: () => getInitialState(initialState),
    reducer: (action, state) =>
      switch action {
      | Change((field, value)) =>
        let data = state.data |> Form.update((field, value));
        switch (field |> getValidator) {
        | None => ReasonReact.Update({...state, data})
        | Some(validator) =>
          let strategy = validator |> getStrategy;
          let emitted = state.emittedFields |> FieldsSet.mem(field);
          switch (strategy, emitted, state.submittedOnce) {
          | (_, true, _)
          | (_, _, true)
          | (Strategy.OnFirstChange, false, false) =>
            switch validator.validateAsync {
            | Some(validateAsync) =>
              switch validator.dependents {
              | Some(dependents) =>
                let (results, emittedFields) =
                  dependents
                  |> validateDependents(
                       ~data,
                       ~results=state.results,
                       ~emittedFields=state.emittedFields
                     );
                data
                |> validator.validate(value)
                |> Validation.ifResult(
                     ~valid=
                       (_) =>
                         ReasonReact.UpdateWithSideEffects(
                           {
                             ...state,
                             data,
                             results: results |> ResultsMap.add(field, None),
                             validating:
                               state.validating |> FieldsSet.add(field),
                             emittedFields:
                               emittedFields |> FieldsSet.add(field)
                           },
                           ({send}) =>
                             send(
                               InvokeDebouncedAsyncValidation(
                                 field,
                                 value,
                                 validateAsync
                               )
                             )
                         ),
                     ~invalid=
                       result =>
                         ReasonReact.Update({
                           ...state,
                           data,
                           results:
                             results |> ResultsMap.add(field, Some(result)),
                           validating:
                             state.validating |> FieldsSet.remove(field),
                           emittedFields: emittedFields |> FieldsSet.add(field)
                         })
                   );
              | None =>
                data
                |> validator.validate(value)
                |> Validation.ifResult(
                     ~valid=
                       (_) =>
                         ReasonReact.UpdateWithSideEffects(
                           {
                             ...state,
                             data,
                             results:
                               state.results |> ResultsMap.add(field, None),
                             validating:
                               state.validating |> FieldsSet.add(field),
                             emittedFields:
                               state.emittedFields |> FieldsSet.add(field)
                           },
                           ({send}) =>
                             send(
                               InvokeDebouncedAsyncValidation(
                                 field,
                                 value,
                                 validateAsync
                               )
                             )
                         ),
                     ~invalid=
                       result =>
                         ReasonReact.Update({
                           ...state,
                           data,
                           results:
                             state.results
                             |> ResultsMap.add(field, Some(result)),
                           validating:
                             state.validating |> FieldsSet.remove(field),
                           emittedFields:
                             state.emittedFields |> FieldsSet.add(field)
                         })
                   )
              }
            | None =>
              switch validator.dependents {
              | Some(dependents) =>
                let result = data |> validator.validate(value);
                let (results, emittedFields) =
                  dependents
                  |> validateDependents(
                       ~data,
                       ~results=state.results,
                       ~emittedFields=state.emittedFields
                     );
                ReasonReact.Update({
                  ...state,
                  data,
                  results: results |> ResultsMap.add(field, Some(result)),
                  emittedFields: emittedFields |> FieldsSet.add(field)
                });
              | None =>
                let result = data |> validator.validate(value);
                ReasonReact.Update({
                  ...state,
                  data,
                  results: state.results |> ResultsMap.add(field, Some(result)),
                  emittedFields: state.emittedFields |> FieldsSet.add(field)
                });
              }
            }
          | (
              Strategy.OnFirstSuccess | Strategy.OnFirstSuccessOrFirstBlur,
              false,
              false
            ) =>
            switch validator.validateAsync {
            | Some(validateAsync) =>
              switch validator.dependents {
              | Some(dependents) =>
                let (results, emittedFields) =
                  dependents
                  |> validateDependents(
                       ~data,
                       ~results=state.results,
                       ~emittedFields=state.emittedFields
                     );
                data
                |> validator.validate(value)
                |> Validation.ifResult(
                     ~valid=
                       (_) =>
                         ReasonReact.UpdateWithSideEffects(
                           {
                             ...state,
                             data,
                             results: results |> ResultsMap.add(field, None),
                             validating:
                               state.validating |> FieldsSet.add(field),
                             emittedFields:
                               emittedFields |> FieldsSet.add(field)
                           },
                           ({send}) =>
                             send(
                               InvokeDebouncedAsyncValidation(
                                 field,
                                 value,
                                 validateAsync
                               )
                             )
                         ),
                     ~invalid=
                       (_) =>
                         ReasonReact.Update({
                           ...state,
                           data,
                           results,
                           emittedFields
                         })
                   );
              | None =>
                data
                |> validator.validate(value)
                |> Validation.ifResult(
                     ~valid=
                       (_) =>
                         ReasonReact.UpdateWithSideEffects(
                           {
                             ...state,
                             data,
                             results:
                               state.results |> ResultsMap.add(field, None),
                             validating:
                               state.validating |> FieldsSet.add(field),
                             emittedFields:
                               state.emittedFields |> FieldsSet.add(field)
                           },
                           ({send}) =>
                             send(
                               InvokeDebouncedAsyncValidation(
                                 field,
                                 value,
                                 validateAsync
                               )
                             )
                         ),
                     ~invalid=(_) => ReasonReact.Update({...state, data})
                   )
              }
            | None =>
              data
              |> validator.validate(value)
              |> Validation.ifResult(
                   ~valid=
                     result =>
                       switch validator.dependents {
                       | Some(dependents) =>
                         let (results, emittedFields) =
                           validateDependents(
                             ~data,
                             ~results=state.results,
                             ~emittedFields=state.emittedFields,
                             dependents
                           );
                         ReasonReact.Update({
                           ...state,
                           data,
                           results:
                             results |> ResultsMap.add(field, Some(result)),
                           emittedFields: emittedFields |> FieldsSet.add(field)
                         });
                       | None =>
                         ReasonReact.Update({
                           ...state,
                           data,
                           results:
                             state.results
                             |> ResultsMap.add(field, Some(result)),
                           emittedFields:
                             state.emittedFields |> FieldsSet.add(field)
                         })
                       },
                   ~invalid=
                     (_) =>
                       switch validator.dependents {
                       | Some(dependents) =>
                         let (results, emittedFields) =
                           validateDependents(
                             ~data,
                             ~results=state.results,
                             ~emittedFields=state.emittedFields,
                             dependents
                           );
                         ReasonReact.Update({
                           ...state,
                           data,
                           results,
                           emittedFields
                         });
                       | None => ReasonReact.Update({...state, data})
                       }
                 )
            }
          | (Strategy.OnFirstBlur | Strategy.OnSubmit, false, false) =>
            ReasonReact.Update({...state, data})
          };
        };
      | Blur((field, value)) =>
        let validator = field |> getValidator;
        let emitted = state.emittedFields |> FieldsSet.mem(field);
        switch (validator, emitted) {
        | (None, _)
        | (Some(_), true) => ReasonReact.NoUpdate
        | (Some(validator), false) =>
          switch (validator |> getStrategy) {
          | Strategy.OnFirstChange
          | Strategy.OnFirstSuccess
          | Strategy.OnSubmit => ReasonReact.NoUpdate
          | Strategy.OnFirstBlur
          | Strategy.OnFirstSuccessOrFirstBlur =>
            switch validator.validateAsync {
            | Some(validateAsync) =>
              state.data
              |> validator.validate(value)
              |> Validation.ifResult(
                   ~valid=
                     (_) =>
                       ReasonReact.UpdateWithSideEffects(
                         {
                           ...state,
                           results:
                             state.results |> ResultsMap.add(field, None),
                           validating: state.validating |> FieldsSet.add(field),
                           emittedFields:
                             state.emittedFields |> FieldsSet.add(field)
                         },
                         ({send}) =>
                           send(
                             InvokeDebouncedAsyncValidation(
                               field,
                               value,
                               validateAsync
                             )
                           )
                       ),
                   ~invalid=
                     result =>
                       ReasonReact.Update({
                         ...state,
                         results:
                           state.results |> ResultsMap.add(field, Some(result)),
                         validating:
                           state.validating |> FieldsSet.remove(field),
                         emittedFields:
                           state.emittedFields |> FieldsSet.add(field)
                       })
                 )
            | None =>
              let result = state.data |> validator.validate(value);
              ReasonReact.Update({
                ...state,
                results: state.results |> ResultsMap.add(field, Some(result)),
                emittedFields: state.emittedFields |> FieldsSet.add(field)
              });
            }
          }
        };
      | InvokeDebouncedAsyncValidation(field, value, validateAsync) =>
        ReasonReact.SideEffects(
          (self => self |> validateAsync(~field, ~value))
        )
      | TriggerAsyncValidation(field, value, validateAsync) =>
        ReasonReact.SideEffects(
          (
            ({send}) =>
              Js.Promise.(
                value
                |> validateAsync
                |> then_(result => {
                     send(ApplyAsyncResult(field, value, result));
                     resolve();
                   })
                |> ignore
              )
          )
        )
      | ApplyAsyncResult(field, value, result) =>
        value === (state.data |> Form.get(field)) ?
          ReasonReact.Update({
            ...state,
            results: state.results |> ResultsMap.add(field, Some(result)),
            validating: state.validating |> FieldsSet.remove(field),
            emittedFields: state.emittedFields |> FieldsSet.add(field)
          }) :
          ReasonReact.NoUpdate
      | Submit =>
        switch (state.validating |> FieldsSet.isEmpty, state.submitting) {
        | (false, _)
        | (_, true) => ReasonReact.NoUpdate
        | _ =>
          let (valid, results) =
            (true, state.results)
            |> Form.Validators.fold(
                 (field', validator', (valid', results')) => {
                   let currentResultInvalid =
                     switch (results' |> ResultsMap.get(field')) {
                     | Some(Validation.Valid(false)) => true
                     | Some(Validation.ValidityBag(bag)) when ! bag.valid =>
                       true
                     | _ => false
                     };
                   let nextResult =
                     state.data
                     |> validator'.validate(state.data |> Form.get(field'));
                   let results =
                     switch (
                       currentResultInvalid,
                       nextResult,
                       validator'.validateAsync
                     ) {
                     | (true, Validation.Valid(true), Some(_)) => results'
                     | (true, Validation.ValidityBag(bag), Some(_))
                         when bag.valid => results'
                     | (_, _, _) =>
                       results' |> ResultsMap.add(field', Some(nextResult))
                     };
                   switch (valid', results |> ResultsMap.get(field')) {
                   | (false, _) => (false, results)
                   | (true, Some(Validation.Valid(valid))) => (valid, results)
                   | (true, Some(Validation.ValidityBag(bag))) => (
                       bag.valid,
                       results
                     )
                   | (_, None) => raise(NoResultInResultsMapOnSubmit(field'))
                   };
                 },
                 debouncedValidators
               );
          valid ?
            ReasonReact.UpdateWithSideEffects(
              {...state, results, submitting: true, submittedOnce: true},
              /* TODO: notifyOnFailure should accept errors */
              (
                ({state, send}) =>
                  onSubmit(
                    state.data,
                    {
                      onSuccess: () => send(Reset),
                      onFailure: () => send(HandleSubmissionError)
                    }
                  )
              )
            ) :
            ReasonReact.Update({
              ...state,
              results,
              submitting: false,
              submittedOnce: true
            });
        }
      | Reset => ReasonReact.Update(initialState |> getInitialState)
      | HandleSubmissionError =>
        ReasonReact.Update({...state, submitting: false})
      },
    render: ({state, send}) =>
      children({
        state: state.data,
        results: field => state.results |> ResultsMap.get(field),
        validating: field => state.validating |> FieldsSet.mem(field),
        submitting: state.submitting,
        change: (field, event) =>
          send(Change((field, event |> Utils.formEventTargetValue))),
        blur: (field, event) =>
          send(Blur((field, event |> Utils.focusEventTargetValue))),
        submit: event => {
          if (! ReactEventRe.Form.defaultPrevented(event)) {
            event |> ReactEventRe.Form.preventDefault;
          };
          send(Submit);
        }
      })
  };
};
