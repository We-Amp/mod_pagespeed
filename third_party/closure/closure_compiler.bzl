"""Rule to download the closure compiler."""

def _closure_compiler_impl(ctx):
    output_dir = ctx.actions.declare_directory("compiler")

    ctx.actions.run_shell(
        outputs = [output_dir],
        tools = [ctx.executable._download_script],
        command = "{script} {output}".format(
            script = ctx.executable._download_script.path,
            output = output_dir.path,
        ),
        use_default_shell_env = True,
    )

    return [DefaultInfo(files = depset([output_dir]))]

closure_compiler = rule(
    implementation = _closure_compiler_impl,
    attrs = {
        "_download_script": attr.label(
            default = Label("//third_party/closure:download.sh"),
            executable = True,
            cfg = "exec",
            allow_single_file = True,
        ),
    },
)
