# fixture: invented python (a tools\ file found by its bare name)


def first_step(x):
    y = x + 1
    return y


class Counter:
    def bump(self, n):
        self.n = getattr(self, 'n', 0) + n
        return self.n
