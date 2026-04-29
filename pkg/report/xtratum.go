// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package report

import (
	"regexp"
)

type xtratum struct {
	*config
}

func ctorXtratum(cfg *config) (reporterImpl, []string, error) {
	ctx := &xtratum{config: cfg}
	return ctx, nil, nil
}

func (ctx *xtratum) ContainsCrash(output []byte) bool {
	return containsCrash(output, xtratumOopses, ctx.ignores)
}

func (ctx *xtratum) Parse(output []byte) *Report {
	return simpleLineParser(output, xtratumOopses, xtratumStackParams, ctx.ignores)
}

func (ctx *xtratum) Symbolize(rep *Report) error {
	return nil
}

var xtratumStackParams = &stackParams{}

var xtratumOopses = append([]*oops{
	{
		[]byte("crash:"),
		[]oopsFormat{
			{
				title: compile("xtratum crash: (.+)"),
				fmt:   "crash: %[1]v",
			},
		},
		[]*regexp.Regexp{},
	},
	&groupGoRuntimeErrors,
}, commonOopses...)
