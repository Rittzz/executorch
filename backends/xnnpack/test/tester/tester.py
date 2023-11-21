# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

import copy
import sys
from abc import ABC, abstractmethod
from collections import OrderedDict
from typing import Any, Dict, List, Optional, Tuple, Type

import torch
import torch._export as export
from executorch import exir
from executorch.backends.xnnpack.partition.xnnpack_partitioner import XnnpackPartitioner
from executorch.backends.xnnpack.passes import XNNPACKPassManager
from executorch.backends.xnnpack.utils.configs import get_xnnpack_edge_compile_config
from executorch.exir import (
    CaptureConfig,
    EdgeCompileConfig,
    ExecutorchBackendConfig,
    ExecutorchProgram,
    ExirExportedProgram,
)
from executorch.exir.backend.backend_api import to_backend, validation_disabled
from executorch.exir.backend.partitioner import Partitioner
from executorch.exir.passes.spec_prop_pass import SpecPropPass
from executorch.exir.print_program import pretty_print, print_program

from executorch.extension.pybindings.portable_lib import (  # @manual
    _load_for_executorch_from_buffer,
)
from torch._export.pass_base import PassType
from torch.ao.quantization.quantize_pt2e import convert_pt2e, prepare_pt2e
from torch.ao.quantization.quantizer.quantizer import Quantizer
from torch.ao.quantization.quantizer.xnnpack_quantizer import (
    get_symmetric_quantization_config,
    XNNPACKQuantizer,
)
from torch.ao.quantization.quantizer.xnnpack_quantizer_utils import QuantizationConfig
from torch.testing import FileCheck
from torch.utils._pytree import tree_flatten


class Stage(ABC):
    """
    Interface for a Stage in the PT2.0 lowering pipeline
    """

    @abstractmethod
    def run(self, artifact, inputs):
        """
        Executes this stage, generates the 'artifact', for later stages.
        """
        pass

    @property
    @abstractmethod
    def artifact(self):
        """
        Returns the artifact generated by this stage. To be used by the next stage in the pipeline.
        """
        pass

    @property
    @abstractmethod
    def graph_module(self):
        """
        Return the artifact's graph module for this stage
        """
        pass

    def run_artifact(self, inputs):
        """
        Returns the output of calling the artifact generated by this stage with inputs
        """
        return self.artifact(*inputs)

    # Debug Tools for stages
    def artifact_str(self):
        """
        Return string printable artifact for this stage
        """
        if isinstance(self.artifact, ExirExportedProgram):
            return self.artifact.exported_program
        return self.artifact

    def stage_banner(self):
        """
        Returns banner string for this stage
        """
        return "#" * 36 + " " + str(self.__class__.__name__) + " " + "#" * 36 + "\n"

    def dump_artifact(self, path_to_dump: Optional[str]):
        """
        Dumps string printable artifact to path. If path_to_dump, then it is printed to terminal
        """
        if path_to_dump:
            with open(path_to_dump, "a") as fp:
                fp.write(str(self.stage_banner() + "\n"))
                fp.write(str(self.artifact_str()))
        else:
            print(self.stage_banner() + "\n")
            print(self.artifact_str())


_stages_: Dict[str, Stage] = {}


def register_stage(stage: Stage):
    """
    Register a Stage to be used in the Tester.
    """
    assert isinstance(stage, type)
    name = stage.__qualname__
    if name in _stages_:
        raise RuntimeError(f"Duplicate stage in Tester, {name}")
    _stages_[name] = stage
    return stage


@register_stage
class Quantize(Stage):
    def __init__(
        self,
        quantizer: Optional[Quantizer] = None,
        quantization_config: Optional[QuantizationConfig] = None,
    ):
        self.quantizer = quantizer or XNNPACKQuantizer()
        self.quantization_config = (
            quantization_config or get_symmetric_quantization_config()
        )

        self.quantizer.set_global(self.quantization_config)

        self.converted_graph = None

    def run(
        self, artifact: torch.nn.Module, inputs: Optional[Tuple[torch.Tensor]]
    ) -> None:
        captured_graph = export.capture_pre_autograd_graph(artifact, inputs)
        prepared = prepare_pt2e(captured_graph, self.quantizer)
        converted = convert_pt2e(prepared)
        self.converted_graph = converted

    @property
    def artifact(self) -> torch.fx.GraphModule:
        return self.converted_graph

    @property
    def graph_module(self) -> str:
        return self.converted_graph


@register_stage
class Export(Stage):
    def __init__(self, capture_config: Optional[CaptureConfig] = None):
        self.capture_conf = capture_config or CaptureConfig(enable_aot=True)
        self.exir_exported_program = None

    def run(self, artifact: torch.nn.Module, inputs) -> None:
        self.exir_exported_program = exir.capture(artifact, inputs, self.capture_conf)

    @property
    def artifact(self) -> ExirExportedProgram:
        return self.exir_exported_program

    @property
    def graph_module(self) -> str:
        return self.exir_exported_program.exported_program.graph_module


@register_stage
class ToEdge(Stage):
    def __init__(self, edge_compile_config: Optional[EdgeCompileConfig] = None):
        self.edge_compile_conf = (
            edge_compile_config or get_xnnpack_edge_compile_config()
        )
        self.edge_dialect_program = None

    def run(self, artifact: ExirExportedProgram, inputs=None) -> None:
        self.edge_dialect_program = artifact.to_edge(self.edge_compile_conf)

    @property
    def artifact(self) -> ExirExportedProgram:
        return self.edge_dialect_program

    @property
    def graph_module(self) -> str:
        return self.edge_dialect_program.exported_program.graph_module


@register_stage
class RunPasses(Stage):
    def __init__(self, pass_list: Optional[List[Type[PassType]]] = None):
        self.pass_list = pass_list
        self.edge_dialect_program = None

    def run(self, artifact: ExirExportedProgram, inputs=None) -> None:
        pass_manager = XNNPACKPassManager(artifact.exported_program, self.pass_list)
        self.edge_dialect_program = artifact
        self.edge_dialect_program.exported_program = pass_manager.transform()

    @property
    def artifact(self) -> ExirExportedProgram:
        return self.edge_dialect_program

    @property
    def graph_module(self) -> str:
        return self.edge_dialect_program.exported_program.graph_module


@register_stage
class Partition(Stage):
    def __init__(self, partitioner: Optional[Partitioner] = None):
        self.partitioner = partitioner or XnnpackPartitioner()
        self.delegate_module = None

    def run(self, artifact: ExirExportedProgram, inputs=None):
        with validation_disabled():
            self.delegate_module = artifact
            self.delegate_module.exported_program = to_backend(
                artifact.exported_program, self.partitioner
            )

    @property
    def artifact(self) -> ExirExportedProgram:
        return self.delegate_module

    @property
    def graph_module(self) -> str:
        return self.delegate_module.exported_program.graph_module


@register_stage
class ToExecutorch(Stage):
    def __init__(
        self,
        config: Optional[ExecutorchBackendConfig] = None,
    ):
        self.config = config or ExecutorchBackendConfig(
            passes=[SpecPropPass()],
        )
        self.executorch_program = None

    def run(self, artifact: ExirExportedProgram, inputs=None):
        self.executorch_program = artifact.to_executorch(self.config)

    @property
    def artifact(self) -> ExecutorchProgram:
        return self.executorch_program

    @property
    def graph_module(self) -> str:
        return self.executorch_program.graph_module

    def dump_artifact(self, path_to_dump: Optional[str]):
        """
        dump_artifact is overridden to dump the serialized program
        """
        original_stdout = sys.stdout

        sys.stdout = open(path_to_dump, "a") if path_to_dump else sys.stdout
        print(self.stage_banner() + "\n")
        pretty_print(self.artifact.program)
        print_program(
            self.artifact.program,
            show_meminfo=True,
            mark_dynamic_shape_tensor=True,
        )
        sys.stdout = original_stdout


@register_stage
class Serialize(Stage):
    def __init__(self):
        self.buffer = None

    def run(self, artifact: ExecutorchProgram, inputs=None) -> None:
        self.buffer = artifact.buffer

    @property
    def artifact(self) -> bytes:
        return self.buffer

    @property
    def graph_module(self) -> None:
        return None

    def run_artifact(self, inputs):
        inputs_flattened, _ = tree_flatten(inputs)
        executorch_module = _load_for_executorch_from_buffer(self.buffer)
        executorch_output = copy.deepcopy(
            executorch_module.run_method("forward", tuple(inputs_flattened))
        )
        return executorch_output

    def dump_artifact(self, path_to_dump: Optional[str]):
        """
        dump_artifact is overridden to dump the serialized bytes into pte file
        """
        if not path_to_dump:
            raise RuntimeError("path_to_dump file not provided")
        else:
            with open(path_to_dump, "wb") as f:
                f.write(self.artifact)


class Tester:
    def __init__(
        self,
        module: torch.nn.Module,
        inputs: Tuple[torch.Tensor],
    ):
        self.original_module = module
        self.inputs = inputs
        self.stages: Dict[str, Stage] = OrderedDict.fromkeys(list(_stages_.keys()))
        self.pipeline = {
            self._stage_name(Quantize): [self._stage_name(Export)],
            self._stage_name(Export): [
                self._stage_name(ToEdge),
            ],
            self._stage_name(ToEdge): [
                self._stage_name(Partition),
                self._stage_name(RunPasses),
            ],
            self._stage_name(RunPasses): [self._stage_name(Partition)],
            # TODO Make this Stage optional
            self._stage_name(Partition): [self._stage_name(ToExecutorch)],
            self._stage_name(ToExecutorch): [self._stage_name(Serialize)],
            self._stage_name(Serialize): [],
        }
        assert all(
            stage in self.pipeline for stage in self.stages
        ), "Invalid Tester internal state!"

        # Current stage name
        self.cur: str = ""

        # Reference output from Eager mode
        self.reference_output = None

        # Artifact output from stage
        self.stage_output = None

    @staticmethod
    def _stage_name(stage) -> str:
        t = stage if isinstance(stage, type) else type(stage)
        return t.__qualname__

    def _pre(self, stage):
        name: str = self._stage_name(stage)
        assert isinstance(name, str) and name in self.stages and not self.stages[name]

        last_artifact = self.original_module
        if self.cur:
            assert self.cur in self.pipeline, f"Invalid state: {self.cur}"
            allowed_next_stages = self.pipeline[self.cur]
            assert name in allowed_next_stages, f"Invalid next stage: {name}"
            last_artifact = self.get_artifact()
        self.cur = name
        return last_artifact

    def _post(self, stage):
        name = self._stage_name(stage)
        assert name in self.stages
        self.stages[name] = stage

    def _run_stage(self, stage_instance, inputs=None):
        assert isinstance(stage_instance, Stage)
        prev_stage_artifact = self._pre(stage_instance)
        stage_instance.run(prev_stage_artifact, inputs=inputs)
        self._post(stage_instance)
        return self

    # Stages
    def quantize(self, quantize_stage: Optional[Quantize] = None):
        return self._run_stage(quantize_stage or Quantize(), self.inputs)

    def export(self, export_stage: Optional[Export] = None):
        return self._run_stage(export_stage or Export(), self.inputs)

    def to_edge(self, to_edge_stage: Optional[ToEdge] = None):
        return self._run_stage(to_edge_stage or ToEdge())

    def run_passes(self, run_passes_stage: Optional[RunPasses] = None):
        return self._run_stage(run_passes_stage or RunPasses())

    def partition(self, partition_stage: Optional[Partition] = None):
        return self._run_stage(partition_stage or Partition())

    def to_executorch(self, to_executorch_stage: Optional[ToExecutorch] = None):
        return self._run_stage(to_executorch_stage or ToExecutorch())

    def serialize(self, serialize_stage: Optional[Serialize] = None):
        return self._run_stage(serialize_stage or Serialize())

    # Util functions
    def dump_artifact(self, path: Optional[str] = None, stage: Optional[str] = None):
        stage = stage or self.cur
        self.stages[stage].dump_artifact(path)
        return self

    def get_artifact(self, stage: Optional[str] = None):
        stage = stage or self.cur
        return self.stages[stage].artifact

    def check(self, input: List[str]):
        for key in input:
            FileCheck().check(key).run(self.stages[self.cur].graph_module.code)
        return self

    def check_not(self, input: List[str]):
        for key in input:
            FileCheck().check_not(key).run(self.stages[self.cur].graph_module.code)
        return self

    def check_count(self, input: Dict[Any, int]):
        # TODO target checks similar to checkGraphModuleNodes()
        for key, count in input.items():
            FileCheck().check_count(key, count, exactly=True).run(
                self.stages[self.cur].graph_module.code
            )
        return self

    def run_method(
        self, stage: Optional[str] = None, inputs: Optional[Tuple[torch.Tensor]] = None
    ):
        inputs_to_run = inputs or self.inputs
        # Reference Output
        self.reference_output = self.stages[self._stage_name(Export)].run_artifact(
            inputs_to_run
        )

        # Output from running artifact at stage
        stage = stage or self.cur
        self.stage_output = self.stages[stage].run_artifact(inputs_to_run)

        return self

    @staticmethod
    def _assert_outputs_equal(model_output, ref_output, atol=1e-03, rtol=1e-03):
        """
        Helper testing function that asserts that the model output and the reference output
        are equal with some tolerance. Due to numerical differences between eager mode and
        the XNNPACK's backend, we relax the detal such that absolute tolerance is 1e-3. and
        relative tolerance is 1e-3.
        """

        # Multiple outputs executor always returns tuple, even if there is one output
        assert len(ref_output) == len(model_output)
        for i in range(len(ref_output)):
            assert torch.allclose(
                model_output[i],
                ref_output[i],
                atol=atol,
                rtol=rtol,
            )

    def compare_outputs(self, atol=1e-03, rtol=1e-03):
        """
        Compares the original of the original nn module with the output of the generated artifact.
        This requres calling run_method before calling compare_outputs. As that runs the generated
        artifact on the sample inputs and sets the stage output to be compared against the reference
        """
        assert self.reference_output is not None
        assert self.stage_output is not None

        # Wrap both outputs as tuple, since executor output is always a tuple even if single tensor
        if isinstance(self.reference_output, torch.Tensor):
            self.reference_output = (self.reference_output,)
        if isinstance(self.stage_output, torch.Tensor):
            self.stage_output = (self.stage_output,)
        self._assert_outputs_equal(
            self.stage_output, self.reference_output, atol=atol, rtol=rtol
        )
        return self
