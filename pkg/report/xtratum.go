// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package report

type xtratum struct {
	*config
}

func ctorXtratum(cfg *config) (reporterImpl, []string, error) {
	ctx := &xtratum{config: cfg}
	return ctx, nil, nil
}

func (ctx *xtratum) ContainsCrash(output []byte) bool {
	return false
}

func (ctx *xtratum) Parse(output []byte) *Report {
	return nil
}

func (ctx *xtratum) Symbolize(rep *Report) error {
	return nil
}
